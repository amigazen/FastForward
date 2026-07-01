/*
 * SPDX-License-Identifier: BSD-2-Clause
 * SPDX-FileCopyrightText: Copyright 2026 amigazen project
 *
 * Copyright (c) 2026, amigazen project
 * All rights reserved.
 */

/*
 * play.c - FastForward Play (CLI and Workbench).
 *
 * A small but real Intuition application that exercises the high-level
 * Movie API. 
 *
 *   - The SAS/C C-runtime startup opens DOSBase and parses the CLI
 *     command line into argc/argv before main() is entered.
 *   - main() distinguishes Workbench (argc == 0, argv is WBStartup)
 *     from Shell launch and dispatches accordingly.
 *   - On Shell launch, ReadArgs(template, opts, NULL) parses
 *     "FILE,DEBUG/S" against the standard CLI input. FILE is the
 *     optional media path, DEBUG/S enables a runtime verbose trace
 *     emitted via PlayLogDebug() / inline `if (PlayDebug) Printf(...)`
 *     in syslog style: "<LEVEL> Play: <message>".
 *   - PlayOpenLibs / PlayCloseLibs handle Intuition / Graphics / ASL
 *     / FastForward; DOSBase is owned by the C runtime, never by us.
 *   - PlayOpenWindow puts up an Intuition window with a Project /
 *     Transport menu (Open, Quit, Play, Pause, Stop, Rewind,
 *     Fast forward, Reverse). The window's RastPort and BitMap are
 *     handed to the movie via FFMT_Window / FFMT_RastPort /
 *     FFMT_BitMap so any future video sink has a render target;
 *     audio-only streams ignore those tags.
 *   - Main loop waits on (window IDCMP | movie signal | Ctrl-C)
 *     so the UI never busy-loops.
 *
 * Workbench-launch via WBStartup (sm_ArgList) is supported. Drag-and-
 * drop of a project icon onto the Play tool is also supported.
 *
 * Usage:
 *     Play                          (CLI - opens ASL requester)
 *     Play foo.wav                  (CLI - direct, positional)
 *     Play FILE foo.wav             (CLI - direct, with template name)
 *     Play foo.wav DEBUG            (CLI - verbose trace)
 *     Workbench: drop icon on tool  (project paths picked up from WB)
 */

/*
 * stdio.h is included solely for sprintf() in the Inspector window's
 * stats formatter (PlayInspectorRefresh). TODO: can be replaced with VSNPrintF()
 */
#include <stdio.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>

#include <dos/dos.h>
#include <dos/dosextens.h>

#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>

#include <graphics/gfx.h>
#include <graphics/rastport.h>

#include <libraries/asl.h>

/*
 * tapedeck.gadget (V47 in OS 3.2, V39 minimum to open) provides the
 * animated tape-controller BOOPSI gadget we plumb into the player's
 * window for play / pause / rewind / fast-forward / scrub. See
 * NDK3.2R4/Autodocs/tapedeck_gc.doc and the
 * NDK3.2R4/Examples/TapeDeck.c reference. 
 */
#include <gadgets/tapedeck.h>

#include <workbench/startup.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/asl.h>

#include <libraries/fastforward.h>
#include <fastforward/ff_string.h>
#include <proto/fastforward.h>

/* ------------------------------------------------------------------ */
/* Library bases                                                       */
/*                                                                      */
/* DOSBase is provided by the SAS/C C-runtime startup before main()    */
/* runs. The other library bases are app-owned and opened/closed in this file.   */
/* ------------------------------------------------------------------ */
struct IntuitionBase  *IntuitionBase = NULL;
struct GfxBase        *GfxBase = NULL;
struct Library        *AslBase = NULL;
struct Library        *TapeDeckBase = NULL;
struct Library        *FastForwardBase = NULL;
static LONG            PlayDebug = FALSE;
static LONG            PlayNoPlay = FALSE;
/*
 * When TRUE, PlayFromPath opens the Inspector window immediately after
 * the main window comes up. Set from CLI STATS=STATISTICS/S or from the
 * Workbench tool type STATS (see PlayFromWB).
 */
static LONG            PlayStartWithInspector = FALSE;


/* ------------------------------------------------------------------ */
/* Syslog-style log helpers                                            */
/*                                                                      */
/* Output format mimics RFC 3164 / classic BSD syslog rendering, just */
/* without the timestamp/host fields (we run on a single machine):    */
/*                                                                      */
/*   <LEVEL> <component>: <message>                                   */
/*                                                                      */
/* Examples:                                                            */
/*                                                                      */
/*   DEBUG Play: opened intuition.library base=12345                  */
/*   INFO  Play: tracks=2                                              */
/*   WARN  Play: no asl.library, file requester disabled              */
/*   ERROR Play: cannot open intuition.library v37                    */
/*                                                                      */
/* The helpers use the standard Amiga vararg trick (passing the args  */
/* array as &fmt+1) so they work under SAS/C without pulling in       */
/* stdarg.h. DEBUG output is gated by the runtime PlayDebug flag.     */
/* ------------------------------------------------------------------ */
static void
PlayLogDebug(CONST_STRPTR fmt, ...)
{
    if (PlayDebug == FALSE) {
        return;
    }
    Printf((STRPTR)"DEBUG Play: ");
    VPrintf((STRPTR)fmt, (LONG *)((&fmt) + 1));
    Printf((STRPTR)"\n");
}

static void
PlayLogInfo(CONST_STRPTR fmt, ...)
{
    Printf((STRPTR)"INFO  Play: ");
    VPrintf((STRPTR)fmt, (LONG *)((&fmt) + 1));
    Printf((STRPTR)"\n");
}

static void
PlayLogWarn(CONST_STRPTR fmt, ...)
{
    Printf((STRPTR)"WARN  Play: ");
    VPrintf((STRPTR)fmt, (LONG *)((&fmt) + 1));
    Printf((STRPTR)"\n");
}

static void
PlayLogError(CONST_STRPTR fmt, ...)
{
    Printf((STRPTR)"ERROR Play: ");
    VPrintf((STRPTR)fmt, (LONG *)((&fmt) + 1));
    Printf((STRPTR)"\n");
}

/*
 * Read the engine's last structured error and print it. Called after
 * any LVO that may have failed (OpenFFMovieA, ProbeFFMediaA, etc.).
 * Quiet if no error is pending.
 */
static void
PlayDumpLastError(CONST_STRPTR where)
{
    struct FFErrorInfo info;
    LONG rc;

    info.ei_Code = 0;
    info.ei_Subsystem = 0;
    info.ei_Operation = 0;
    info.ei_Component = NULL;
    info.ei_Function = NULL;
    info.ei_Message[0] = '\0';

    rc = GetFFLastError(&info);
    if (rc == 0 && info.ei_Code == 0) {
        Printf((STRPTR)"ERROR Play: %s: no structured error pending\n",
            (LONG)where);
        return;
    }

    Printf((STRPTR)"ERROR Play: %s: code=%ld component=%s function=%s\n",
        (LONG)where,
        info.ei_Code,
        info.ei_Component != NULL ? (LONG)info.ei_Component : (LONG)"",
        info.ei_Function != NULL ? (LONG)info.ei_Function : (LONG)"");
    if (info.ei_Message[0] != '\0') {
        Printf((STRPTR)"ERROR Play: %s: msg: %s\n",
            (LONG)where, (LONG)info.ei_Message);
    }
}

/* ------------------------------------------------------------------ */
/* Application state                                                    */
/* ------------------------------------------------------------------ */
struct PlayApp {
    struct Window    *pa_Window;
    struct Menu      *pa_Menu;
    struct FFMovie   *pa_Movie;
    /*
     * pa_TapeDeck is a BOOPSI Object* allocated via NewObject() against
     * the "tapedeck.gadget" class. It doubles as a struct Gadget* under
     * the hood (every BOOPSI gadget class derives from gadgetclass via
     * propgclass) and is therefore both DisposeObject'd at teardown and
     * AddGList()'d into pa_Window's gadget list at OpenWindow time.
     * NULL when the optional tapedeck.gadget class library is not
     * present, in which case the player still works via menus only.
     */
    APTR              pa_TapeDeck;
    /*
     * Last slider value pushed to or read from the gadget. Used in
     * two ways: (a) the playback-follower in PlayTapeDeckRefresh-
     * FromMovie() compares against it to suppress redundant
     * SetGadgetAttrs() calls when the engine position has not
     * advanced; (b) the GADGETUP handler compares against it to
     * detect "the user moved the slider this click" without needing
     * a separate scrub-armed flag - the gadget itself tells us.
     */
    LONG              pa_LastSliderValue;
    /*
     * Inspector ("stats for nerds") second window. Toggled via
     * Project / Inspector... from the menu strip. NULL when closed -
     * which is the default state - so the main loop can skip the
     * inspector's signal bit / IDCMP drain without branching.
     * pa_InspectorSigMask is the cached `1UL << UserPort.mp_SigBit`
     * for the inspector window so PlayMainLoop() does not have to
     * touch the Window struct on every Wait().
     */
    struct Window    *pa_Inspector;
    ULONG             pa_InspectorSigMask;
    UBYTE             pa_Path[256];
    UBYTE             pa_Drawer[256];
    UBYTE             pa_File[108];
};

/* ------------------------------------------------------------------ */
/* Menu layout                                                          */
/*                                                                      */
/* Two menus: Project (Open / Quit) and Transport (Play / Pause / Stop  */
/* / Rewind / Fast forward / Reverse). The dispatcher picks them up by  */
/* (MENUNUM, ITEMNUM) from IDCMP code, since classic Intuition          */
/* MenuItem has no UserData field and we deliberately avoid pulling in  */
/* GadTools just to layout six items.                                   */
/* ------------------------------------------------------------------ */
#define MENU_PROJECT     0
#define MENU_TRANSPORT   1

#define ITEM_OPEN        0
#define ITEM_INSPECT     1
#define ITEM_QUIT        2

#define ITEM_PLAY        0
#define ITEM_PAUSE       1
#define ITEM_STOP        2
#define ITEM_REWIND      3
#define ITEM_FFWD        4
#define ITEM_REVERSE     5

static struct IntuiText IT_Open    = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Open...",     NULL };
static struct IntuiText IT_Inspect = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Inspector...",NULL };
static struct IntuiText IT_Quit    = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Quit",        NULL };
static struct IntuiText IT_Play    = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Play",        NULL };
static struct IntuiText IT_Pause   = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Pause",       NULL };
static struct IntuiText IT_Stop    = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Stop",        NULL };
static struct IntuiText IT_Rewind  = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Rewind",      NULL };
static struct IntuiText IT_Ffwd    = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Fast forward",NULL };
static struct IntuiText IT_Reverse = { 0,1,JAM2,0,1,NULL,(UBYTE *)"Reverse",     NULL };

/*
 * Ordering follows ITEM_* above so MENUNUM/ITEMNUM dispatch lines up.
 * Each item's NextSelect is MENUNULL because we are not using
 * multi-select chaining.
 */
static struct MenuItem MI_Reverse = {
    NULL, 0,72,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Reverse, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Ffwd = {
    &MI_Reverse, 0,60,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Ffwd, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Rewind = {
    &MI_Ffwd, 0,48,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Rewind, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Stop = {
    &MI_Rewind, 0,36,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Stop, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Pause = {
    &MI_Stop, 0,24,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Pause, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Play = {
    &MI_Pause, 0,12,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Play, NULL, 0, NULL, MENUNULL
};

static struct MenuItem MI_Quit = {
    NULL, 0,24,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Quit, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Inspect = {
    &MI_Quit, 0,12,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Inspect, NULL, 0, NULL, MENUNULL
};
static struct MenuItem MI_Open = {
    &MI_Inspect, 0,0,160,10,
    ITEMTEXT|ITEMENABLED|HIGHCOMP, 0,
    (APTR)&IT_Open, NULL, 0, NULL, MENUNULL
};

static struct Menu M_Transport = {
    NULL, 70,0,80,0,
    MENUENABLED, (BYTE *)"Transport", &MI_Play
};
static struct Menu M_Project = {
    &M_Transport, 0,0,70,0,
    MENUENABLED, (BYTE *)"Project", &MI_Open
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */
static LONG  PlayOpenLibs(void);
static void  PlayCloseLibs(void);
static LONG  PlayOpenWindow(struct PlayApp *app);
static void  PlayCloseWindow(struct PlayApp *app);
static LONG  PlayPickFile(struct PlayApp *app);
static LONG  PlayOpenMovie(struct PlayApp *app, CONST_STRPTR path);
static void  PlayCloseMovie(struct PlayApp *app);
static LONG  PlayResizeWindowForVideo(struct PlayApp *app);
static void  PlayRebuildTapeDeckAfterResize(struct PlayApp *app);
static void  PlayDescribeTracks(struct FFMovie *movie);
static void  PlayDrawStatus(struct PlayApp *app, CONST_STRPTR text);
static ULONG PlayHandleIDCMP(struct PlayApp *app);
static void  PlayPumpEvents(struct PlayApp *app);
static LONG  PlayMainLoop(struct PlayApp *app);
static LONG  PlayFromPath(CONST_STRPTR path);
static LONG  PlayFromWB(struct WBStartup *wb);

static LONG  PlayTapeDeckCreate(struct PlayApp *app);
static void  PlayTapeDeckDispose(struct PlayApp *app);
static void  PlayTapeDeckOnMovieOpen(struct PlayApp *app);
static void  PlayTapeDeckOnMovieClose(struct PlayApp *app);
static void  PlayTapeDeckHandleEvent(struct PlayApp *app,
                                     struct IntuiMessage *im);
static void  PlayTapeDeckHandleScrub(struct PlayApp *app);
static void  PlayTapeDeckRefreshFromMovie(struct PlayApp *app);

static LONG  PlayInspectorOpen(struct PlayApp *app);
static void  PlayInspectorClose(struct PlayApp *app);
static ULONG PlayInspectorHandleIDCMP(struct PlayApp *app);
static void  PlayInspectorRefresh(struct PlayApp *app);

/* ------------------------------------------------------------------ */
/* Library opens                                                        */
/* ------------------------------------------------------------------ */
static LONG
PlayOpenLibs(void)
{
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary((STRPTR)"intuition.library", 37UL);
    if (IntuitionBase == NULL) {
        PlayLogError("cannot open intuition.library v37");
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: opened intuition.library base=%lx\n",
            (ULONG)IntuitionBase);
    }

    GfxBase = (struct GfxBase *)
        OpenLibrary((STRPTR)"graphics.library", 37UL);
    if (GfxBase == NULL) {
        PlayLogError("cannot open graphics.library v37");
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: opened graphics.library base=%lx\n",
            (ULONG)GfxBase);
    }

    AslBase = OpenLibrary((STRPTR)"asl.library", 37UL);
    if (AslBase == NULL) {
        /*
         * ASL is optional - the player can still launch via CLI or WB
         * without it; we just lose the file requester fallback.
         */
        PlayLogWarn("no asl.library, file requester disabled");
    } else {
        if (PlayDebug) {
            Printf("DEBUG Play: opened asl.library base=%lx\n",
                (ULONG)AslBase);
        }
    }

    /*
     * gadgets/tapedeck.gadget is the BOOPSI class that gives us the
     * animated transport row (rewind / play-or-pause / fast forward
     * + a position slider) seen in animation.datatype-aware viewers
     * to 47 in OS 3.2 but the NDK Example builds it against v39, so
     * we open at v39 to keep the player compatible with OS 3.5 / 3.9
     * installations too. The library is optional - if it cannot be
     * opened we silently fall back to the existing Transport menu.
     */
    TapeDeckBase = OpenLibrary((STRPTR)"gadgets/tapedeck.gadget", 39UL);
    if (TapeDeckBase == NULL) {
        PlayLogWarn("no tapedeck.gadget v39, transport controls"
            " menu-only");
    } else {
        if (PlayDebug) {
            Printf("DEBUG Play: opened tapedeck.gadget base=%lx\n",
                (ULONG)TapeDeckBase);
        }
    }

    FastForwardBase = FF_OpenLibrary(FF_API_VERSION);
    if (FastForwardBase == NULL) {
        PlayLogError("cannot open %s v%ld", FASTFORWARDNAME, FF_API_VERSION);
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: opened %s base=%lx api=%ld\n",
            FASTFORWARDNAME, (ULONG)FastForwardBase,
            (ULONG)FF_API_VERSION);
    }

    /*
     * Crank the engine log level to DEBUG when the play app was
     * launched with DEBUG/S so library-side breadcrumbs (sink picker,
     * pipeline counters, audio.sink open/push/close) actually print
     * to our shell. Without this the library's fh_Log threshold sits
     * at FFLOG_NONE and stays silent.
     */
    if (PlayDebug) {
        SetFFLogLevel(FFLOG_DEBUG);
    }

    PlayLogDebug("loading FastForward plugins");
    LoadFFPlugins(NULL);
    PlayLogDebug("FastForward plugins loaded");
    return RETURN_OK;
}

static void
PlayCloseLibs(void)
{
    if (FastForwardBase != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: unloading FastForward plugins\n");
        }
        UnloadFFPlugins();
        if (PlayDebug) {
            Printf("DEBUG Play: closing %s base=%lx\n",
                FASTFORWARDNAME, (ULONG)FastForwardBase);
        }
        FF_CloseLibrary(FastForwardBase);
        FastForwardBase = NULL;
    }
    if (TapeDeckBase != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing tapedeck.gadget base=%lx\n",
                (ULONG)TapeDeckBase);
        }
        CloseLibrary(TapeDeckBase);
        TapeDeckBase = NULL;
    }
    if (AslBase != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing asl.library base=%lx\n",
                (ULONG)AslBase);
        }
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
    if (GfxBase != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing graphics.library base=%lx\n",
                (ULONG)GfxBase);
        }
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (IntuitionBase != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing intuition.library base=%lx\n",
                (ULONG)IntuitionBase);
        }
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                               */
/* ------------------------------------------------------------------ */
static LONG
PlayOpenWindow(struct PlayApp *app)
{
    struct NewWindow nw;

    if (PlayDebug) {
        Printf("DEBUG Play: opening window left=%ld top=%ld width=%ld height=%ld\n",
            40UL, 40UL, 360UL, 115UL);
    }

    nw.LeftEdge   = 40;
    nw.TopEdge    = 40;
    nw.Width      = 360;
    nw.Height     = 115;
    nw.DetailPen  = 0;
    nw.BlockPen   = 1;
    /*
     * IDCMP additions for the tape-deck gadget:
     *   IDCMP_GADGETUP    - button release (a tape-deck button was
     *                       hit, or the slider was dropped)
     *   IDCMP_MOUSEMOVE   - emitted while the slider is being dragged
     *                       (we requested GA_FollowMouse on the
     *                       gadget object). We treat this as a scrub
     *                       event and forward to SeekFFMovie() on the
     *                       fly so the user hears playback follow the
     *                       knob.
     *   IDCMP_INTUITICKS  - low-rate (~10 Hz) tick used to refresh
     *                       the slider position from the movie clock
     *                       while the mouse is over the window. The
     *                       movie's own signal mask drives updates
     *                       while it is running independently; ticks
     *                       just keep the UI snappy when the user is
     *                       interacting with the player.
     */
    nw.IDCMPFlags = IDCMP_CLOSEWINDOW
                  | IDCMP_MENUPICK
                  | IDCMP_VANILLAKEY
                  | IDCMP_REFRESHWINDOW
                  | IDCMP_GADGETUP
                  | IDCMP_MOUSEMOVE
                  | IDCMP_INTUITICKS
                  /*
                   * IDCMP_NEWSIZE drives the post-open "adapt window to
                   * video" path: PlayOpenMovie() queries the new movie's
                   * FFMT_Width / FFMT_Height and calls ChangeWindowBox()
                   * to grow (or shrink) the window. ChangeWindowBox()
                   * processes asynchronously; the IDCMP_NEWSIZE message
                   * Intuition posts when the new box is in effect is our
                   * signal to rebuild the bottom-anchored tape-deck
                   * gadget against the now-current Width / Height.
                   */
                  | IDCMP_NEWSIZE;
    nw.Flags      = WFLG_DRAGBAR
                  | WFLG_DEPTHGADGET
                  | WFLG_CLOSEGADGET
                  | WFLG_ACTIVATE
                  | WFLG_SMART_REFRESH
                  | WFLG_NOCAREREFRESH;
    nw.FirstGadget = NULL;
    nw.CheckMark   = NULL;
    nw.Title       = (UBYTE *)"FastForward Play";
    nw.Screen      = NULL;
    nw.BitMap      = NULL;
    nw.MinWidth    = 240;
    nw.MinHeight   = 80;
    nw.MaxWidth    = 0;
    nw.MaxHeight   = 0;
    nw.Type        = WBENCHSCREEN;

    app->pa_Window = OpenWindow(&nw);
    if (app->pa_Window == NULL) {
        PlayLogError("cannot open Intuition window");
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: window opened window=%lx userport=%lx sigbit=%ld rport=%lx\n",
            (ULONG)app->pa_Window,
            (ULONG)app->pa_Window->UserPort,
            (ULONG)app->pa_Window->UserPort->mp_SigBit,
            (ULONG)app->pa_Window->RPort);
    }

    app->pa_Menu = &M_Project;
    if (SetMenuStrip(app->pa_Window, app->pa_Menu) == FALSE) {
        PlayLogError("cannot attach menus");
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: menu strip attached menu=%lx\n",
            (ULONG)app->pa_Menu);
    }

    /*
     * Bring up the tape-deck gadget. PlayTapeDeckCreate() is a no-op
     * when tapedeck.gadget is unavailable so callers can rely on
     * pa_TapeDeck being either a usable BOOPSI object or NULL.
     */
    PlayTapeDeckCreate(app);

    PlayDrawStatus(app, "Ready - choose Project / Open... or play <file>");
    return RETURN_OK;
}

static void
PlayCloseWindow(struct PlayApp *app)
{
    if (app->pa_Window != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing window window=%lx\n",
                (ULONG)app->pa_Window);
        }
        /*
         * Close the Inspector first; it holds no resource that depends
         * on the main window but the user expects both to disappear
         * together when the app exits. Doing it here also keeps the
         * inspector's signal bit out of any final Wait() the main
         * loop happens to do during teardown.
         */
        PlayInspectorClose(app);
        /*
         * Dispose the tape-deck *before* CloseWindow() so Intuition
         * never sees a dangling Gadget pointer in the window's
         * FirstGadget list. PlayTapeDeckDispose() calls RemoveGList()
         * under the lock, then DisposeObject() releases the BOOPSI
         * object via the class library.
         */
        PlayTapeDeckDispose(app);
        if (app->pa_Menu != NULL) {
            ClearMenuStrip(app->pa_Window);
            app->pa_Menu = NULL;
        }
        CloseWindow(app->pa_Window);
        app->pa_Window = NULL;
    }
}

static void
PlayDrawStatus(struct PlayApp *app, CONST_STRPTR text)
{
    struct RastPort *rp;
    LONG y;

    if (PlayDebug && text != NULL) {
        Printf("DEBUG Play: status text '%s'\n", text);
    }

    if (app->pa_Window == NULL || text == NULL) {
        return;
    }

    rp = app->pa_Window->RPort;
    /*
     * Status row is anchored against the bottom of the window, just
     * above the tape-deck gadget, so the full BorderTop..bottom-of-
     * status-row vertical band is available as a video render area.
     * Keep this Y constant in lockstep with PlayTapeDeckCreate()'s
     * gy math: the tape-deck top edge sits at
     *     Window->Height - BorderBottom - tapeH - 6
     * (tapeH = 16 in PlayTapeDeckCreate); we want the topaz text
     * baseline to land 4 px above that, with the 11 px rectfill that
     * encloses the glyph leaving a 2-px gap between text and gadget.
     *
     *   tape_top = Height - BorderBottom - 16 - 6 = Height - BB - 22
     *   text baseline y = tape_top - 4              = Height - BB - 26
     *
     * Audio-only windows (which never trigger PlayResizeWindowFor-
     * Video) get the same bottom layout - one less code path to
     * special-case, and a consistent "status near the controls" UX.
     */
    y = (LONG)app->pa_Window->Height
        - (LONG)app->pa_Window->BorderBottom - 26L;

    SetAPen(rp, 0);
    RectFill(rp,
        (LONG)app->pa_Window->BorderLeft + 4,
        y - 9,
        (LONG)app->pa_Window->Width
            - (LONG)app->pa_Window->BorderRight - 4,
        y + 2);
    SetAPen(rp, 1);
    Move(rp, (LONG)app->pa_Window->BorderLeft + 6, y);
    Text(rp, (STRPTR)text, (LONG)FFStrLen(text));
}

/* ------------------------------------------------------------------ */
/* ASL file requester                                                   */
/* ------------------------------------------------------------------ */
static LONG
PlayPickFile(struct PlayApp *app)
{
    struct FileRequester *fr;
    LONG ok;

    if (PlayDebug) {
        Printf("DEBUG Play: opening ASL file requester aslbase=%lx window=%lx\n",
            (ULONG)AslBase, (ULONG)app->pa_Window);
    }

    if (AslBase == NULL || app->pa_Window == NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: ASL requester unavailable\n");
        }
        return RETURN_FAIL;
    }

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_Window,        (ULONG)app->pa_Window,
        ASLFR_TitleText,     (ULONG)"Open media file",
        ASLFR_PositiveText,  (ULONG)"Play",
        ASLFR_RejectIcons,   TRUE,
        TAG_DONE);
    if (fr == NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: AllocAslRequestTags failed\n");
        }
        return RETURN_FAIL;
    }

    ok = (LONG)AslRequest(fr, NULL);
    if (PlayDebug) {
        Printf("DEBUG Play: AslRequest returned %ld drawer='%s' file='%s'\n",
            ok,
            fr->fr_Drawer != NULL ? fr->fr_Drawer : (STRPTR)"",
            fr->fr_File != NULL ? fr->fr_File : (STRPTR)"");
    }
    if (ok != 0 && fr->fr_File != NULL && fr->fr_File[0] != '\0') {
        FFStrCopyN(app->pa_Drawer, (STRPTR)fr->fr_Drawer,
            sizeof(app->pa_Drawer));
        FFStrCopyN(app->pa_File, (STRPTR)fr->fr_File,
            sizeof(app->pa_File));
        FFStrCopyN(app->pa_Path, app->pa_Drawer, sizeof(app->pa_Path));
        AddPart(app->pa_Path, app->pa_File, sizeof(app->pa_Path));
        if (PlayDebug) {
            Printf("DEBUG Play: ASL selected path='%s'\n",
                (STRPTR)app->pa_Path);
        }
        FreeAslRequest(fr);
        return RETURN_OK;
    }
    FreeAslRequest(fr);
    return RETURN_FAIL;
}

/* ------------------------------------------------------------------ */
/* Movie                                                                */
/* ------------------------------------------------------------------ */
static void
PlayDescribeTracks(struct FFMovie *movie)
{
    ULONG count;
    ULONG i;
    struct FFTrack *t;
    ULONG kind;
    STRPTR desc;
    struct FFAudioFormat *af;
    struct TagItem qtags[5];

    count = GetFFTrackCount(movie);
    PlayLogInfo("tracks=%ld", count);
    if (PlayDebug) {
        Printf("DEBUG Play: movie=%lx track_count=%ld\n",
            (ULONG)movie, count);
    }

    for (i = 0; i < count; i++) {
        t = GetFFTrack(movie, i);
        if (t == NULL) {
            if (PlayDebug) {
                Printf("DEBUG Play: track %ld returned NULL\n", i);
            }
            continue;
        }
        kind = 0;
        desc = NULL;
        af = NULL;

        qtags[0].ti_Tag = FFTA_Kind;
        qtags[0].ti_Data = (ULONG)&kind;
        qtags[1].ti_Tag = FFTA_Description;
        qtags[1].ti_Data = (ULONG)&desc;
        qtags[2].ti_Tag = FFTA_AudioFormat;
        qtags[2].ti_Data = (ULONG)&af;
        qtags[3].ti_Tag = TAG_DONE;
        qtags[3].ti_Data = 0;

        GetFFTrackAttrsA(t, qtags);
        if (PlayDebug) {
            Printf("DEBUG Play: track=%ld ptr=%lx kind=%ld desc=%lx audiofmt=%lx\n",
                i, (ULONG)t, kind, (ULONG)desc, (ULONG)af);
        }

        Printf("  [%ld] kind=%ld %s",
            i, kind, desc != NULL ? desc : (STRPTR)"");
        if (af != NULL && af->af_SampleRate != 0) {
            Printf("  %ld Hz x %ld ch %ld bit",
                af->af_SampleRate, af->af_Channels, af->af_Bits);
        }
        Printf("\n");
    }
}

static LONG
PlayOpenMovie(struct PlayApp *app, CONST_STRPTR path)
{
    struct TagItem mtags[10];
    ULONG i;

    if (PlayDebug) {
        Printf("DEBUG Play: PlayOpenMovie path='%s' window=%lx\n",
            path != NULL ? path : (STRPTR)"", (ULONG)app->pa_Window);
    }

    if (path == NULL) {
        return RETURN_FAIL;
    }

    PlayCloseMovie(app);

    i = 0;
    mtags[i].ti_Tag  = FFMT_Path;
    mtags[i++].ti_Data = (ULONG)path;
    mtags[i].ti_Tag  = FFMT_AutoPlay;
    mtags[i++].ti_Data = PlayNoPlay ? FALSE : TRUE;
    mtags[i].ti_Tag  = FFMT_AHIUnit;
    mtags[i++].ti_Data = 0UL;
    mtags[i].ti_Tag  = FFMT_Volume;
    mtags[i++].ti_Data = 0x10000UL;
    if (app->pa_Window != NULL) {
        /*
         * Render-target hints: any video sink the engine plumbs in for
         * the stream picks these up. Audio-only streams ignore them.
         */
        mtags[i].ti_Tag  = FFMT_Window;
        mtags[i++].ti_Data = (ULONG)app->pa_Window;
        mtags[i].ti_Tag  = FFMT_RastPort;
        mtags[i++].ti_Data = (ULONG)app->pa_Window->RPort;
        if (app->pa_Window->RPort != NULL
            && app->pa_Window->RPort->BitMap != NULL) {
            mtags[i].ti_Tag  = FFMT_BitMap;
            mtags[i++].ti_Data = (ULONG)app->pa_Window->RPort->BitMap;
        }
    }
    mtags[i].ti_Tag  = TAG_DONE;
    mtags[i].ti_Data = 0;

    if (PlayDebug) {
        Printf("DEBUG Play: movie tags path=%lx autoplay=%ld ahiunit=%ld volume=%lx\n",
            mtags[0].ti_Data, mtags[1].ti_Data,
            mtags[2].ti_Data, mtags[3].ti_Data);
        if (app->pa_Window != NULL) {
            Printf("DEBUG Play: movie render tags window=%lx rport=%lx bitmap=%lx\n",
                (ULONG)app->pa_Window,
                (ULONG)app->pa_Window->RPort,
                app->pa_Window->RPort != NULL
                    ? (ULONG)app->pa_Window->RPort->BitMap : 0UL);
        }
        Printf("DEBUG Play: calling OpenFFMovieA\n");
        Flush(Output());
    }

    app->pa_Movie = OpenFFMovieA(mtags);
    if (PlayDebug) {
        Flush(Output());
    }
    if (app->pa_Movie == NULL) {
        PlayLogError("cannot open movie: %s", path);
        PlayDumpLastError("OpenFFMovieA");
        if (PlayDebug) {
            Printf("DEBUG Play: OpenFFMovieA failed path='%s'\n", path);
        }
        PlayDrawStatus(app, "Open failed");
        return RETURN_FAIL;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: OpenFFMovieA returned movie=%lx sigmask=%lx state=%ld\n",
            (ULONG)app->pa_Movie,
            GetFFMovieSignalMask(app->pa_Movie),
            GetFFMovieState(app->pa_Movie));
    }

    PlayLogInfo("opened '%s'", path);
    PlayDescribeTracks(app->pa_Movie);
    /*
     * If the new movie carries video, adapt the window to its frame
     * size: PlayResizeWindowForVideo() disposes the tape-deck and
     * queues a ChangeWindowBox(); the IDCMP_NEWSIZE handler in
     * PlayHandleIDCMP() then rebuilds the tape-deck against the
     * resized window and re-seeds it via PlayTapeDeckOnMovieOpen().
     * For audio-only streams the resize is a no-op (RETURN_FAIL) and
     * we configure the tape-deck inline as we always have, so the
     * audio-only code path keeps its exact existing behaviour.
     */
    if (PlayResizeWindowForVideo(app) != RETURN_OK) {
        /*
         * Now that the movie is alive, push its initial state into
         * the tape-deck gadget: head at frame 0, mode Play / Stop
         * depending on whether autoplay was disabled, paused FALSE.
         * For video files this same call is invoked from the
         * IDCMP_NEWSIZE handler after the resize lands.
         */
        PlayTapeDeckOnMovieOpen(app);
    }
    if (PlayNoPlay) {
        PlayDrawStatus(app, (CONST_STRPTR)"Opened");
    } else {
        PlayDrawStatus(app, (CONST_STRPTR)"Playing");
    }
    return RETURN_OK;
}

static void
PlayCloseMovie(struct PlayApp *app)
{
    if (app->pa_Movie != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: closing movie movie=%lx state=%ld\n",
                (ULONG)app->pa_Movie,
                GetFFMovieState(app->pa_Movie));
        }
        CloseFFMovie(app->pa_Movie);
        app->pa_Movie = NULL;
    }
    /*
     * Always reset the gadget after teardown, even when there was
     * never a live movie: the call is NULL-safe internally and
     * keeps the visual deck idle between files.
     */
    PlayTapeDeckOnMovieClose(app);
}

/*
 * Adapt the window to the loaded movie's video dimensions.
 *
 * Layout target (top to bottom inside the window's inner client rect):
 *
 *   [ video render area  ]   vw x vh - video.sink blits the decoded
 *                            frame here at (BorderLeft, BorderTop).
 *                            The C2P / RTG path inside video.sink
 *                            already targets that offset, so as long
 *                            as the window has enough room the full
 *                            frame comes through unclipped.
 *   [ padding             ]  paddingV px gap so the last scanline
 *                            does not touch the status text.
 *   [ status text row    ]   topaz line drawn by PlayDrawStatus(),
 *                            anchored against the window bottom so
 *                            it always sits just above the tape-deck.
 *   [ tape-deck gadget   ]   16 px BOOPSI gadget + 6 px above / 4 px
 *                            below, anchored against the bottom
 *                            border (PlayTapeDeckCreate()'s math).
 *
 * Outer dimensions add the window's own border thicknesses
 * (BorderLeft/Right/Top/Bottom) on top of the inner layout.
 *
 * ChangeWindowBox() requires V36+ Intuition (we already require V37
 * in PlayOpenLibs) and is asynchronous: Intuition will post an
 * IDCMP_NEWSIZE message once Width/Height are valid for the new box.
 * We dispose the tape-deck *before* queueing the box change so the
 * gadget cannot momentarily float inside the newly-enlarged client
 * area between the resize landing and our handler running; the
 * IDCMP_NEWSIZE handler in PlayHandleIDCMP() picks up "tape-deck is
 * NULL" and rebuilds one at the correct bottom edge.
 *
 * Returns RETURN_OK when a resize was queued (caller must defer any
 * tape-deck-mutating work to IDCMP_NEWSIZE), or RETURN_FAIL when the
 * movie has no usable dimensions (audio-only, no FFMT_Width/Height).
 * In the FAIL case the caller keeps the tape-deck as-is and seeds it
 * inline through PlayTapeDeckOnMovieOpen() just like before.
 */
static LONG
PlayResizeWindowForVideo(struct PlayApp *app)
{
    ULONG vw;
    ULONG vh;
    struct TagItem qtags[3];
    struct Screen *scr;
    LONG borderH;
    LONG borderV;
    LONG statusH;
    LONG tapeH;
    LONG paddingV;
    LONG newWidth;
    LONG newHeight;
    LONG newLeft;
    LONG newTop;
    LONG screenW;
    LONG screenH;

    if (app == NULL || app->pa_Window == NULL || app->pa_Movie == NULL) {
        return RETURN_FAIL;
    }

    vw = 0;
    vh = 0;
    qtags[0].ti_Tag = FFMT_Width;
    qtags[0].ti_Data = (ULONG)&vw;
    qtags[1].ti_Tag = FFMT_Height;
    qtags[1].ti_Data = (ULONG)&vh;
    qtags[2].ti_Tag = TAG_DONE;
    qtags[2].ti_Data = 0;
    GetFFMovieAttrsA(app->pa_Movie, qtags);

    if (vw == 0 || vh == 0) {
        if (PlayDebug) {
            Printf("DEBUG Play: resize skipped vw=%lu vh=%lu"
                " (audio-only or no video metadata)\n", vw, vh);
        }
        return RETURN_FAIL;
    }

    /*
     * Border / row math:
     *   borderH/V are Intuition's own title-bar / dragbar / border
     *     thicknesses; stable for the life of the window so reading
     *     them here is safe even though Width / Height will change.
     *   statusH covers PlayDrawStatus()'s topaz row and the 11-px
     *     rectfill it uses to clear stale glyphs (y - 9 .. y + 2).
     *   tapeH is the tape-deck gadget height (16) plus the 6 px above
     *     and 4 px below that PlayTapeDeckCreate() bakes in - keep in
     *     lockstep with the math there.
     *   paddingV is breathing room between the video and the status
     *     row so the last scanline does not touch the topaz baseline.
     */
    borderH = (LONG)app->pa_Window->BorderLeft +
              (LONG)app->pa_Window->BorderRight;
    borderV = (LONG)app->pa_Window->BorderTop +
              (LONG)app->pa_Window->BorderBottom;
    statusH = 18L;
    tapeH = 16L + 10L;
    paddingV = 4L;

    newWidth = (LONG)vw + borderH + 12L;
    newHeight = (LONG)vh + borderV + statusH + tapeH + paddingV;

    /*
     * Clamp to the parent screen so the new window box is reachable
     * and the dragbar / depth gadget stay on-screen after the move.
     * A NULL WScreen would be a custom-screen edge case we have no
     * answer for; fall back to a sane PAL maximum.
     */
    scr = app->pa_Window->WScreen;
    if (scr != NULL) {
        screenW = (LONG)scr->Width;
        screenH = (LONG)scr->Height;
    } else {
        screenW = 640L;
        screenH = 480L;
    }
    if (newWidth > screenW) {
        newWidth = screenW;
    }
    if (newHeight > screenH) {
        newHeight = screenH;
    }
    /*
     * Never go below the per-window minimums we ourselves declared
     * via NewWindow.MinWidth / MinHeight in PlayOpenWindow(); also
     * guarantees the tape-deck has enough horizontal room for the
     * rewind / play / forward buttons + a usable slider on tiny
     * "thumbnail" clips (16 x 16 mascots etc.).
     */
    if (newWidth < 240L) {
        newWidth = 240L;
    }
    if (newHeight < 80L) {
        newHeight = 80L;
    }

    /*
     * Try to keep the current top-left; only nudge the window inwards
     * when the new box would otherwise spill off the right or bottom
     * edge of the screen. The user's current placement is a UX
     * preference we should respect when we can.
     */
    newLeft = (LONG)app->pa_Window->LeftEdge;
    newTop = (LONG)app->pa_Window->TopEdge;
    if (newLeft + newWidth > screenW) {
        newLeft = screenW - newWidth;
    }
    if (newTop + newHeight > screenH) {
        newTop = screenH - newHeight;
    }
    if (newLeft < 0L) {
        newLeft = 0L;
    }
    if (newTop < 0L) {
        newTop = 0L;
    }

    if (PlayDebug) {
        Printf("DEBUG Play: resizing window for video vw=%lu vh=%lu"
            " left=%ld top=%ld width=%ld height=%ld\n",
            vw, vh, newLeft, newTop, newWidth, newHeight);
    }

    PlayTapeDeckDispose(app);
    ChangeWindowBox(app->pa_Window, newLeft, newTop,
        (LONG)newWidth, (LONG)newHeight);
    return RETURN_OK;
}

/*
 * Re-attach a tape-deck gadget at the (now-current) bottom of the
 * window after an IDCMP_NEWSIZE wake-up and seed it with the loaded
 * movie's state. Called from the NEWSIZE handler. Idempotent: if a
 * tape-deck is already attached we still tear it down and rebuild it
 * so its absolute Y position tracks the new Window->Height. The
 * common case (PlayResizeWindowForVideo() disposed first) skips
 * straight to the create path.
 */
static void
PlayRebuildTapeDeckAfterResize(struct PlayApp *app)
{
    if (app == NULL || app->pa_Window == NULL) {
        return;
    }
    if (app->pa_TapeDeck != NULL) {
        /*
         * A previous NEWSIZE (or a future user-driven resize) left
         * the deck in place at the OLD bottom edge. Drop it so the
         * recreate below lands at the current bottom.
         */
        PlayTapeDeckDispose(app);
    }
    PlayTapeDeckCreate(app);
    /*
     * Push the live movie's duration / mode / paused state back into
     * the freshly-created gadget. PlayTapeDeckOnMovieOpen() is the
     * single source of truth for "movie just became playable, mirror
     * that on the deck" so it is the right entry point even though we
     * are running it for the second time on the same movie. The
     * function is NULL-safe when no movie is loaded, so calling it
     * unconditionally is fine.
     */
    PlayTapeDeckOnMovieOpen(app);
}

/* ------------------------------------------------------------------ */
/* Tape-deck gadget                                                     */
/*                                                                      */
/* The tape-deck is created in animation-controls mode (TDECK_Tape =    */
/* TRUE) so we get a rewind / play / fast-forward button group plus a   */
/* slider that can be dragged to scrub through the timeline - the same  */
/* affordances animation.datatype exposes through animation.gadget.     */
/* The slider value space is fixed at 0..PLAY_TAPEDECK_SCRUB_STEPS so   */
/* scrub sensitivity is independent of clip length.                     */
/*                                                                      */
/* Pause is handled through the documented TDECK_Paused attribute: the  */
/* gadget keeps TDECK_Mode at BUT_PLAY while playback is alive and we   */
/* toggle TDECK_Paused on / off when the user re-clicks Play (or hits   */
/* the space bar).                                                      */
/* ------------------------------------------------------------------ */

/*
 * Build the BOOPSI tape-deck gadget and attach it to the live window.
 * No-op when tapedeck.gadget could not be opened in PlayOpenLibs(),
 * so the player still functions through its menus on older systems.
 */
static LONG
PlayTapeDeckCreate(struct PlayApp *app)
{
    APTR gad;
    LONG gx;
    LONG gy;
    LONG gw;
    LONG gh;

    if (app == NULL || app->pa_Window == NULL) {
        return RETURN_FAIL;
    }
    if (TapeDeckBase == NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: tapedeck.gadget unavailable, skipping\n");
        }
        return RETURN_FAIL;
    }

    /*
     * Place the gadget along the lower edge of the window, leaving
     * one character row of padding from the status text above and
     * the bottom border below. Width tracks the window's interior so
     * the slider gets as much travel as possible. The dimensions
     * here mirror the NDK TapeDeck.c example (202 x 15) but scale
     * out to the full inner width of our window.
     */
    gx = (LONG)app->pa_Window->BorderLeft + 6L;
    gw = (LONG)app->pa_Window->Width
        - (LONG)app->pa_Window->BorderLeft
        - (LONG)app->pa_Window->BorderRight - 12L;
    gh = 16L;
    gy = (LONG)app->pa_Window->Height
        - (LONG)app->pa_Window->BorderBottom - gh - 6L;
    if (gw < 120L) {
        gw = 120L;
    }
    if (gy < (LONG)app->pa_Window->BorderTop + 30L) {
        gy = (LONG)app->pa_Window->BorderTop + 30L;
    }

    if (PlayDebug) {
        Printf("DEBUG Play: creating tape-deck gadget left=%ld top=%ld"
            " width=%ld height=%ld\n", gx, gy, gw, gh);
    }

    /*
     * NewObject() against the class library produces an Object* that
     * Intuition will happily treat as a struct Gadget* (the BOOPSI
     * gadgetclass / propgclass hierarchy guarantees a sane
     * gadget-shaped prefix). Tag meanings:
     *   GA_Left/Top/Width/Height: classic gadgetclass placement.
     *   GA_RelVerify  - emit IDCMP_GADGETUP only when the click was
     *                   validated (the user lifted the mouse over
     *                   the gadget).
     *   GA_Immediate  - emit IDCMP_GADGETDOWN on press too, which we
     *                   currently ignore but is harmless and matches
     *                   the NDK Example.
     *   GA_FollowMouse - emit IDCMP_MOUSEMOVE while the gadget is
     *                   active. This drives our scrub-while-dragging
     *                   path in PlayTapeDeckHandleScrub().
     *   TDECK_Tape TRUE = animation controls (rewind / play /
     *                   forward + frame slider). FALSE would give us
     *                   the five-button tape-deck variant (rewind /
     *                   play / forward / stop / pause) without a
     *                   slider - which means no scrub. The user
     *                   wants play/pause/seek/scrobble so we pick
     *                   animation mode here; stop is still available
     *                   via the Transport menu and space bar still
     *                   toggles play/pause.
     *   TDECK_Frames - slider range. Seeds at 0 here and is rewritten
     *                   to GetFFMovieDuration() in PlayTapeDeckOn-
     *                   MovieOpen(). The slider then operates in
     *                   engine tick units directly: TDECK_CurrentFrame
     *                   values feed straight into SeekFFMovie() and
     *                   GetFFMoviePosition() results feed straight
     *                   back. No conversion in this file.
     *   TDECK_CurrentFrame - initial slider position.
     *   TDECK_Mode  - start out in STOP so the play button isn't
     *                   lit before a movie is even loaded.
     */
    gad = NewObject(NULL, (UBYTE *)"tapedeck.gadget",
        GA_Left,          gx,
        GA_Top,           gy,
        GA_Width,         gw,
        GA_Height,        gh,
        GA_ID,            (ULONG)1,
        GA_RelVerify,     TRUE,
        GA_Immediate,     TRUE,
        GA_FollowMouse,   TRUE,
        TDECK_Tape,       TRUE,
        TDECK_Frames,     0UL,
        TDECK_CurrentFrame, 0UL,
        TDECK_Mode,       (ULONG)BUT_STOP,
        TDECK_Paused,     FALSE,
        TAG_DONE);
    if (gad == NULL) {
        PlayLogWarn("NewObject(tapedeck.gadget) failed - menu-only"
            " transport");
        return RETURN_FAIL;
    }

    app->pa_TapeDeck = gad;
    app->pa_LastSliderValue = 0L;

    /*
     * AddGList() injects the gadget into the window's gadget list
     * after the window is already open. Position -1 / count -1 means
     * "append, with at most one gadget". RefreshGList() forces the
     * gadget to render immediately so the user sees it without first
     * having to interact with the window.
     */
    AddGList(app->pa_Window, (struct Gadget *)gad, (UWORD)-1, -1L, NULL);
    RefreshGList((struct Gadget *)gad, app->pa_Window, NULL, 1L);

    if (PlayDebug) {
        Printf("DEBUG Play: tape-deck gadget=%lx attached\n",
            (ULONG)gad);
    }
    return RETURN_OK;
}

/*
 * Tear the gadget down. Always paired with PlayTapeDeckCreate() and
 * tolerant of NULL state, so it can be called unconditionally from
 * PlayCloseWindow() on every exit path.
 */
static void
PlayTapeDeckDispose(struct PlayApp *app)
{
    if (app == NULL || app->pa_TapeDeck == NULL) {
        return;
    }

    if (app->pa_Window != NULL) {
        if (PlayDebug) {
            Printf("DEBUG Play: removing tape-deck gadget=%lx\n",
                (ULONG)app->pa_TapeDeck);
        }
        RemoveGList(app->pa_Window,
            (struct Gadget *)app->pa_TapeDeck, 1L);
    }
    DisposeObject(app->pa_TapeDeck);
    app->pa_TapeDeck = NULL;
    app->pa_LastSliderValue = 0L;
}

/*
 * Push the tape-deck into "we just opened a movie" state: stretch
 * the slider range to match the movie's duration, drop the play
 * head to zero, paint the mode as Play (or Stop in noplay mode).
 *
 * GetFFMovieDuration() returns the clip length in the same engine
 * tick units as GetFFMoviePosition() and SeekFFMovie() consume, so
 * we feed the value through as-is - no remapping. The gadget's
 * slider is then directly addressable in those ticks.
 */
static void
PlayTapeDeckOnMovieOpen(struct PlayApp *app)
{
    UWORD mode;
    ULONG duration;

    if (app == NULL || app->pa_TapeDeck == NULL || app->pa_Window == NULL) {
        return;
    }
    mode = PlayNoPlay ? (UWORD)BUT_STOP : (UWORD)BUT_PLAY;
    duration = (app->pa_Movie != NULL)
        ? GetFFMovieDuration(app->pa_Movie) : 0UL;
    SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
        NULL,
        TDECK_Frames,        duration,
        TDECK_CurrentFrame,  0UL,
        TDECK_Mode,          (ULONG)mode,
        TDECK_Paused,        FALSE,
        TAG_DONE);
    app->pa_LastSliderValue = 0L;
    if (PlayDebug) {
        Printf("DEBUG Play: tape-deck reset for new movie mode=%ld"
            " duration=%lu\n", (ULONG)mode, duration);
    }
}

/*
 * Movie closed: bring the gadget back to Stop / paused=FALSE / head
 * at zero so the next movie starts from a clean visual state.
 */
static void
PlayTapeDeckOnMovieClose(struct PlayApp *app)
{
    if (app == NULL || app->pa_TapeDeck == NULL || app->pa_Window == NULL) {
        return;
    }
    SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
        NULL,
        TDECK_Mode,         (ULONG)BUT_STOP,
        TDECK_Paused,       FALSE,
        TDECK_CurrentFrame, 0UL,
        TAG_DONE);
    app->pa_LastSliderValue = 0L;
}

/*
 * Translate an IDCMP_GADGETUP from the tape-deck into engine LVO
 * calls. The gadget keeps its visual mode in sync internally, but we
 * still SetGadgetAttrs() back any state we want to enforce (for
 * instance, dropping Paused to FALSE after a Play action).
 */
static void
PlayTapeDeckHandleEvent(struct PlayApp *app, struct IntuiMessage *im)
{
    ULONG mode;
    ULONG paused;
    LONG  frame;
    LONG  frameMoved;

    if (app == NULL || app->pa_TapeDeck == NULL || app->pa_Window == NULL) {
        return;
    }
    if (im == NULL || im->IAddress != app->pa_TapeDeck) {
        return;
    }

    mode = (ULONG)BUT_STOP;
    paused = FALSE;
    frame = 0L;
    GetAttr((Tag)TDECK_Mode, app->pa_TapeDeck, &mode);
    GetAttr((Tag)TDECK_Paused, app->pa_TapeDeck, &paused);
    GetAttr((Tag)TDECK_CurrentFrame, app->pa_TapeDeck, (ULONG *)&frame);

    if (PlayDebug) {
        Printf("DEBUG Play: tape-deck GADGETUP mode=%ld paused=%ld"
            " frame=%ld lastSlider=%ld\n",
            mode, paused, frame, app->pa_LastSliderValue);
    }

    if (app->pa_Movie == NULL) {
        /*
         * No movie loaded: rebound the gadget to its idle state and
         * nudge the user to open one. We still honour Open via the
         * Project menu, so this is just a friendly hint.
         */
        SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
            NULL,
            TDECK_Mode,         (ULONG)BUT_STOP,
            TDECK_Paused,       FALSE,
            TDECK_CurrentFrame, 0UL,
            TAG_DONE);
        PlayDrawStatus(app, "Open a file first (Project / Open...)");
        return;
    }

    /*
     * Dispatch is two-axis: the slider can move independently of the
     * button mode (animation tape-deck's frame slider is just a prop
     * gadget that reports the new TDECK_CurrentFrame on release).
     * Handle a slider move first, then act on whichever button was
     * clicked. REWIND / FORWARD also implicitly move the frame so
     * the order is "seek to wherever the gadget says we are, then
     * apply the new mode" - which is exactly what RewindFFMovie()
     * does internally (StopFFMovie + SeekFFMovie(0)).
     */
    frameMoved = (frame != app->pa_LastSliderValue);
    if (frameMoved != FALSE) {
        if (PlayDebug) {
            Printf("DEBUG Play: tape-deck seek frame=%ld\n", frame);
        }
        SeekFFMovie(app->pa_Movie, (ULONG)frame, 0UL);
        app->pa_LastSliderValue = frame;
        PlayDrawStatus(app, "Seeking");
    }

    switch (mode) {
    case BUT_PLAY:
        /*
         * The animation tape-deck re-uses the Play button to toggle
         * between play and pause - TDECK_Paused flips on every click
         * while TDECK_Mode stays at BUT_PLAY. Mirror that in the
         * engine.
         */
        if (paused != FALSE) {
            PauseFFMovie(app->pa_Movie);
            PlayDrawStatus(app, "Paused");
        } else {
            PlayFFMovie(app->pa_Movie);
            PlayDrawStatus(app, "Playing");
        }
        break;
    case BUT_PAUSE:
        PauseFFMovie(app->pa_Movie);
        PlayDrawStatus(app, "Paused");
        break;
    case BUT_STOP:
        StopFFMovie(app->pa_Movie);
        PlayDrawStatus(app, "Stopped");
        break;
    case BUT_REWIND:
    case BUT_BEGIN:
        /*
         * RewindFFMovie() is StopFFMovie + SeekFFMovie(0). The
         * gadget already pushed TDECK_CurrentFrame to 0 when the
         * user clicked Rewind, so the seek above already happened
         * at 0; this just brings the engine state in line and
         * updates the status text.
         */
        RewindFFMovie(app->pa_Movie);
        app->pa_LastSliderValue = 0L;
        SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
            NULL,
            TDECK_CurrentFrame, 0UL,
            TAG_DONE);
        PlayDrawStatus(app, "Rewound");
        break;
    case BUT_FORWARD:
    case BUT_END:
        FastForwardFFMovie(app->pa_Movie);
        PlayDrawStatus(app, "Fast forward");
        break;
    default:
        if (PlayDebug) {
            Printf("DEBUG Play: tape-deck unhandled mode=%ld\n", mode);
        }
        break;
    }
}

/*
 * Mouse-move handler used while the user is dragging the slider.
 * Reads the current TDECK_CurrentFrame and forwards the result to
 * SeekFFMovie() so the user can hear playback follow the knob in
 * real time, instead of waiting for the GADGETUP edge.
 */
static void
PlayTapeDeckHandleScrub(struct PlayApp *app)
{
    LONG frame;
    LONG selected;

    if (app == NULL || app->pa_TapeDeck == NULL ||
        app->pa_Window == NULL || app->pa_Movie == NULL) {
        return;
    }

    selected = ((struct Gadget *)app->pa_TapeDeck)->Flags & GFLG_SELECTED;
    if (selected == 0L) {
        return;
    }

    frame = 0L;
    GetAttr((Tag)TDECK_CurrentFrame, app->pa_TapeDeck, (ULONG *)&frame);
    if (frame == app->pa_LastSliderValue) {
        return;
    }

    app->pa_LastSliderValue = frame;
    if (PlayDebug) {
        Printf("DEBUG Play: tape-deck scrub frame=%ld\n", frame);
    }
    SeekFFMovie(app->pa_Movie, (ULONG)frame, 0UL);
}

/*
 * Pull the live playback position from the engine and push it back
 * into the slider when the user is not actively scrubbing. Throttled
 * by pa_LastSliderValue so we never invoke SetGadgetAttrs() unless
 * the value would change - SetGadgetAttrs() on the prop-derived
 * tape-deck triggers a partial redraw and we want to avoid those on
 * every tick.
 */
static void
PlayTapeDeckRefreshFromMovie(struct PlayApp *app)
{
    ULONG position;
    LONG  selected;
    ULONG state;

    if (app == NULL || app->pa_TapeDeck == NULL ||
        app->pa_Window == NULL || app->pa_Movie == NULL) {
        return;
    }
    selected = ((struct Gadget *)app->pa_TapeDeck)->Flags & GFLG_SELECTED;
    if (selected != 0L) {
        return;  /* user is dragging - don't fight them */
    }

    position = GetFFMoviePosition(app->pa_Movie);
    if ((LONG)position == app->pa_LastSliderValue) {
        return;
    }
    app->pa_LastSliderValue = (LONG)position;

    /*
     * Reflect the engine's PLAYING / PAUSED / FINISHED state on the
     * gadget too so the lit button matches what the user actually
     * hears. We only need this on finish - the gadget's own button
     * state from a user click already matches PLAYING / PAUSED.
     */
    state = GetFFMovieState(app->pa_Movie);
    if (state == FFMS_FINISHED) {
        SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
            NULL,
            TDECK_CurrentFrame, position,
            TDECK_Mode,         (ULONG)BUT_STOP,
            TDECK_Paused,       FALSE,
            TAG_DONE);
    } else {
        SetGadgetAttrs((struct Gadget *)app->pa_TapeDeck, app->pa_Window,
            NULL,
            TDECK_CurrentFrame, position,
            TAG_DONE);
    }
}

/* ------------------------------------------------------------------ */
/* Inspector window ("stats for nerds")                                 */
/*                                                                      */
/* A second, optional, top-level Intuition window. Opened on Project /  */
/* Inspector... menu pick; closed on its own CLOSEGADGET or when the    */
/* main window goes away. The window has no gadgets - it draws plain    */
/* topaz text via Move/Text on every IDCMP_INTUITICKS and on every      */
/* PlayMainLoop wake-up, sourcing values from the engine via the new   */
/* FFMT_Pipeline* / FFMT_VideoFps / FFMT_*Bitrate read-only attrs.      */
/*                                                                      */
/* The implementation is deliberately small: no GadTools, no complex     */
/* layout, no double-buffered redraw. The whole window is repainted on  */
/* every tick because the wall of text fits in one frame and the       */
/* refresh cadence is human-perceivable (~10Hz), not animation-speed.  */
/* ------------------------------------------------------------------ */
static LONG
PlayInspectorOpen(struct PlayApp *app)
{
    struct NewWindow nw;

    if (app == NULL) {
        return RETURN_FAIL;
    }
    if (app->pa_Inspector != NULL) {
        /*
         * Already up - bring to front and re-activate. Mirrors what
         * any sane app would do if the user picks the same menu item
         * twice; we deliberately do NOT close-and-reopen because that
         * would race the main loop's signal mask.
         */
        WindowToFront(app->pa_Inspector);
        ActivateWindow(app->pa_Inspector);
        return RETURN_OK;
    }

    if (PlayDebug) {
        Printf("DEBUG Play: opening inspector window\n");
    }

    nw.LeftEdge   = 80;
    nw.TopEdge    = 80;
    nw.Width      = 380;
    nw.Height     = 230;
    nw.DetailPen  = 0;
    nw.BlockPen   = 1;
    /*
     * IDCMP set deliberately narrow: CLOSE for the user dismissing
     * the window, REFRESH so we can repaint after damage, and
     * INTUITICKS to drive the ~10Hz redraw without consuming CPU.
     * The main window's IDCMP set is unchanged.
     */
    nw.IDCMPFlags = IDCMP_CLOSEWINDOW
                  | IDCMP_REFRESHWINDOW
                  | IDCMP_INTUITICKS;
    nw.Flags      = WFLG_DRAGBAR
                  | WFLG_DEPTHGADGET
                  | WFLG_CLOSEGADGET
                  | WFLG_SMART_REFRESH
                  | WFLG_NOCAREREFRESH;
    nw.FirstGadget = NULL;
    nw.CheckMark   = NULL;
    nw.Title       = (UBYTE *)"FastForward Inspector";
    nw.Screen      = NULL;
    nw.BitMap      = NULL;
    nw.MinWidth    = 280;
    nw.MinHeight   = 120;
    nw.MaxWidth    = 0;
    nw.MaxHeight   = 0;
    nw.Type        = WBENCHSCREEN;

    app->pa_Inspector = OpenWindow(&nw);
    if (app->pa_Inspector == NULL) {
        PlayLogWarn("cannot open inspector window");
        app->pa_InspectorSigMask = 0UL;
        return RETURN_FAIL;
    }
    /*
     * Cache the inspector's wake-up bit so PlayMainLoop's Wait()
     * does not have to chase pointers through Window->UserPort
     * every iteration. Cleared back to 0 when we close the window
     * so the OR into the wait mask becomes a no-op automatically.
     */
    app->pa_InspectorSigMask =
        1UL << app->pa_Inspector->UserPort->mp_SigBit;
    if (PlayDebug) {
        Printf("DEBUG Play: inspector opened win=%lx sigMask=%lx\n",
            (ULONG)app->pa_Inspector, app->pa_InspectorSigMask);
    }
    PlayInspectorRefresh(app);
    return RETURN_OK;
}

static void
PlayInspectorClose(struct PlayApp *app)
{
    if (app == NULL || app->pa_Inspector == NULL) {
        return;
    }
    if (PlayDebug) {
        Printf("DEBUG Play: closing inspector window=%lx\n",
            (ULONG)app->pa_Inspector);
    }
    CloseWindow(app->pa_Inspector);
    app->pa_Inspector = NULL;
    app->pa_InspectorSigMask = 0UL;
}

/*
 * Drain the inspector window's IDCMP port. Mirrors the main window's
 * PlayHandleIDCMP() structurally - cache the relevant fields into a
 * stack-local, ReplyMsg() the original immediately, then dispatch -
 * so behaviour stays predictable even when the user spams events.
 * Returns 0 always; the inspector is a secondary window and never
 * asks the app to quit.
 */
static ULONG
PlayInspectorHandleIDCMP(struct PlayApp *app)
{
    struct IntuiMessage *im;
    ULONG class;
    UBYTE closeRequested;

    closeRequested = FALSE;

    if (app == NULL || app->pa_Inspector == NULL) {
        return 0UL;
    }

    while ((im = (struct IntuiMessage *)GetMsg(
            app->pa_Inspector->UserPort)) != NULL) {
        class = im->Class;
        ReplyMsg((struct Message *)im);

        switch (class) {
        case IDCMP_CLOSEWINDOW:
            closeRequested = TRUE;
            break;
        case IDCMP_REFRESHWINDOW:
            /*
             * NOCAREREFRESH means Intuition still posts the event but
             * does NOT pre-clear; we pair BeginRefresh/EndRefresh and
             * then redraw the stats so nothing is left half-cleared.
             */
            BeginRefresh(app->pa_Inspector);
            EndRefresh(app->pa_Inspector, TRUE);
            PlayInspectorRefresh(app);
            break;
        case IDCMP_INTUITICKS:
            PlayInspectorRefresh(app);
            break;
        default:
            break;
        }
    }

    if (closeRequested != FALSE) {
        PlayInspectorClose(app);
    }
    return 0UL;
}

/*
 * Pretty-print a sample count in clock units (FFMT_TimeScale ticks) as
 * "S.fff" seconds into a caller-supplied buffer. Used for position /
 * duration / time-stamp displays in the inspector. Avoids floats so
 * SAS/C does not link the libm shim just to render hh:mm:ss.fff.
 */
static void
PlayInspectorFormatTime(STRPTR dst, ULONG cap,
    ULONG ticks, ULONG timescale)
{
    ULONG seconds;
    ULONG millis;
    if (dst == NULL || cap == 0) {
        return;
    }
    if (timescale == 0) {
        sprintf(dst, "n/a");
        return;
    }
    seconds = ticks / timescale;
    /*
     * Multiply BEFORE divide for the milliseconds part to keep one
     * decimal of precision on the integer divide. For typical
     * timescales (1000 or 90000) this fits comfortably in a ULONG.
     */
    millis = ((ticks % timescale) * 1000UL) / timescale;
    sprintf(dst, "%lu.%03lu s", seconds, millis);
}

/*
 * Render one Inspector text row. Always clears the row's row-rect
 * first so leftover digits from a longer previous value never
 * "ghost" behind a shorter new one (a classic Intuition gotcha
 * when you redraw text without an enclosing RectFill).
 */
static void
PlayInspectorDrawRow(struct PlayApp *app, LONG row, CONST_STRPTR text)
{
    struct RastPort *rp;
    LONG x;
    LONG y;
    LONG xRight;

    if (app == NULL || app->pa_Inspector == NULL || text == NULL) {
        return;
    }
    rp = app->pa_Inspector->RPort;
    x = (LONG)app->pa_Inspector->BorderLeft + 6;
    /*
     * topaz 8 advances 10 px per row including descender clearance.
     * Row 0 sits one line below the title bar.
     */
    y = (LONG)app->pa_Inspector->BorderTop + 10 + row * 10;
    xRight = (LONG)app->pa_Inspector->Width
           - (LONG)app->pa_Inspector->BorderRight - 4;

    SetAPen(rp, 0);
    RectFill(rp, x - 2, y - 8, xRight, y + 1);
    SetAPen(rp, 1);
    Move(rp, x, y);
    Text(rp, (STRPTR)text, (LONG)FFStrLen(text));
}

/*
 * Pretty-print one pipeline-slot row to the supplied buffer. Returns
 * a tag string ("SRC ", "DMX ", "COD ", "SNK ") so the caller can
 * skip the row entirely when the element is NULL. Stats counters
 * are fetched via GetFFElementAttrsA - element pointers come from
 * the FFMT_Pipeline* attrs queried by the caller.
 */
static void
PlayInspectorFormatPipelineRow(STRPTR dst, ULONG cap, CONST_STRPTR tag,
    struct FFElement *e)
{
    STRPTR desc;
    ULONG  kind;
    ULONG  buffersOut;
    ULONG  bytesOut;
    ULONG  buffersIn;
    ULONG  bytesIn;
    ULONG  bytesShown;
    CONST_STRPTR unit;
    struct TagItem etags[7];

    if (dst == NULL || cap == 0) {
        return;
    }
    if (e == NULL) {
        sprintf(dst, "%s -", tag);
        return;
    }

    desc = NULL;
    kind = 0;
    buffersOut = 0;
    bytesOut   = 0;
    buffersIn  = 0;
    bytesIn    = 0;

    etags[0].ti_Tag = FFE_Description;  etags[0].ti_Data = (ULONG)&desc;
    etags[1].ti_Tag = FFE_Kind;         etags[1].ti_Data = (ULONG)&kind;
    etags[2].ti_Tag = FFE_BuffersOut;   etags[2].ti_Data = (ULONG)&buffersOut;
    etags[3].ti_Tag = FFE_BytesOut;     etags[3].ti_Data = (ULONG)&bytesOut;
    etags[4].ti_Tag = FFE_BuffersIn;    etags[4].ti_Data = (ULONG)&buffersIn;
    etags[5].ti_Tag = FFE_BytesIn;      etags[5].ti_Data = (ULONG)&bytesIn;
    etags[6].ti_Tag = TAG_DONE;         etags[6].ti_Data = 0;
    GetFFElementAttrsA(e, etags);

    /*
     * For sink rows the meaningful counter is BytesIn (what flowed
     * INTO the sink for playback). For source / demux / codec rows
     * BytesOut is meaningful (what flowed OUT to the next element).
     * Both are bumped so we just prefer the larger non-zero value -
     * keeps this helper agnostic to the element's kind.
     */
    bytesShown = (bytesOut >= bytesIn) ? bytesOut : bytesIn;

    /*
     * Show MB once we cross ~1 MiB, otherwise KiB. Avoids zero-row
     * displays for short files where bytes counters are <1 KiB, but
     * keeps headline numbers readable for long files.
     */
    if (bytesShown >= (1UL << 20)) {
        bytesShown = bytesShown >> 20;
        unit = "MB";
    } else if (bytesShown >= (1UL << 10)) {
        bytesShown = bytesShown >> 10;
        unit = "KB";
    } else {
        unit = "B ";
    }

    sprintf(dst, "%s %-16s %5lu buf  %5lu %s",
        tag,
        (desc != NULL) ? (char *)desc : "(unnamed)",
        (buffersOut > 0) ? buffersOut : buffersIn,
        bytesShown,
        unit);
}

/*
 * Drop-rate tracking. We snapshot the cumulative drop count from the
 * previous refresh together with the movie position at that moment,
 * then divide the deltas to display drops per second. Using the
 * movie's own timescale-aware position avoids needing a separate
 * timer.device open just for the inspector's status line.
 */
static ULONG PlayInspectorPrevDropped;
static ULONG PlayInspectorPrevPosition;
static ULONG PlayInspectorPrevTimeScale;

static void
PlayInspectorRefresh(struct PlayApp *app)
{
    char buf[96];
    char fmt1[40];
    char fmt2[40];
    LONG row;
    /* Movie-wide stats */
    ULONG state;
    ULONG position;
    ULONG duration;
    ULONG timescale;
    ULONG sampleRate;
    ULONG channels;
    ULONG width;
    ULONG height;
    LONG  videoFps;
    ULONG videoBitrate;
    ULONG audioBitrate;
    ULONG videoFrames;
    ULONG droppedFrames;
    ULONG clockKind;
    LONG  avDrift;
    ULONG dropDelta;
    ULONG posDelta;
    struct FFElement *src;
    struct FFElement *demux;
    struct FFElement *codec;
    struct FFElement *audioSink;
    struct FFElement *videoSink;
    struct TagItem mtags[20];
    ULONG i;
    CONST_STRPTR stateStr;
    /*
     * Hoisted out of the videoFps-display branch below: C89 disallows
     * mid-block declarations and the project rule reinforces that any
     * conditional block must not introduce variables. We declare here
     * even when the branch may not run; the assignment is trivial.
     */
    ULONG fpsWhole;
    ULONG fpsFrac;
    LONG  driftWhole;
    LONG  driftFrac;
    LONG  driftAbs;

    if (app == NULL || app->pa_Inspector == NULL) {
        return;
    }

    state          = 0;
    position       = 0;
    duration       = 0;
    timescale      = 0;
    sampleRate     = 0;
    channels       = 0;
    width          = 0;
    height         = 0;
    videoFps       = 0;
    videoBitrate   = 0;
    audioBitrate   = 0;
    videoFrames    = 0;
    droppedFrames  = 0;
    clockKind      = 0;
    avDrift        = 0;
    dropDelta      = 0;
    posDelta       = 0;
    src            = NULL;
    demux          = NULL;
    codec          = NULL;
    audioSink      = NULL;
    videoSink      = NULL;

    if (app->pa_Movie != NULL) {
        i = 0;
        mtags[i].ti_Tag = FFMT_State;        mtags[i].ti_Data = (ULONG)&state; i++;
        mtags[i].ti_Tag = FFMT_Position;     mtags[i].ti_Data = (ULONG)&position; i++;
        mtags[i].ti_Tag = FFMT_Duration;     mtags[i].ti_Data = (ULONG)&duration; i++;
        mtags[i].ti_Tag = FFMT_TimeScale;    mtags[i].ti_Data = (ULONG)&timescale; i++;
        mtags[i].ti_Tag = FFMT_SampleRate;   mtags[i].ti_Data = (ULONG)&sampleRate; i++;
        mtags[i].ti_Tag = FFMT_Channels;     mtags[i].ti_Data = (ULONG)&channels; i++;
        mtags[i].ti_Tag = FFMT_Width;        mtags[i].ti_Data = (ULONG)&width; i++;
        mtags[i].ti_Tag = FFMT_Height;       mtags[i].ti_Data = (ULONG)&height; i++;
        mtags[i].ti_Tag = FFMT_VideoFps;     mtags[i].ti_Data = (ULONG)&videoFps; i++;
        mtags[i].ti_Tag = FFMT_VideoBitrate; mtags[i].ti_Data = (ULONG)&videoBitrate; i++;
        mtags[i].ti_Tag = FFMT_AudioBitrate; mtags[i].ti_Data = (ULONG)&audioBitrate; i++;
        mtags[i].ti_Tag = FFMT_VideoFrames;  mtags[i].ti_Data = (ULONG)&videoFrames; i++;
        mtags[i].ti_Tag = FFMT_DroppedFrames; mtags[i].ti_Data = (ULONG)&droppedFrames; i++;
        mtags[i].ti_Tag = FFMT_ClockKind;     mtags[i].ti_Data = (ULONG)&clockKind; i++;
        mtags[i].ti_Tag = FFMT_AVDrift;       mtags[i].ti_Data = (ULONG)&avDrift; i++;
        mtags[i].ti_Tag = FFMT_PipelineSource;    mtags[i].ti_Data = (ULONG)&src; i++;
        mtags[i].ti_Tag = FFMT_PipelineDemux;     mtags[i].ti_Data = (ULONG)&demux; i++;
        mtags[i].ti_Tag = FFMT_PipelineCodec;     mtags[i].ti_Data = (ULONG)&codec; i++;
        mtags[i].ti_Tag = FFMT_PipelineAudioSink; mtags[i].ti_Data = (ULONG)&audioSink; i++;
        mtags[i].ti_Tag = FFMT_PipelineVideoSink; mtags[i].ti_Data = (ULONG)&videoSink; i++;
        mtags[i].ti_Tag = TAG_DONE;          mtags[i].ti_Data = 0; i++;
        GetFFMovieAttrsA(app->pa_Movie, mtags);
    }

    row = 0;

    if (app->pa_Movie == NULL) {
        PlayInspectorDrawRow(app, row++, "No movie loaded.");
        PlayInspectorDrawRow(app, row++,
            "Open one with Project / Open... or play <file> on the");
        PlayInspectorDrawRow(app, row++,
            "command line; stats will populate live.");
        return;
    }

    switch (state) {
    case FFMS_OPENED:    stateStr = "OPENED";    break;
    case FFMS_PREROLL:   stateStr = "PREROLL";   break;
    case FFMS_PLAYING:   stateStr = "PLAYING";   break;
    case FFMS_PAUSED:    stateStr = "PAUSED";    break;
    case FFMS_STOPPED:   stateStr = "STOPPED";   break;
    case FFMS_FINISHED:  stateStr = "FINISHED";  break;
    case FFMS_ERROR:     stateStr = "ERROR";     break;
    default:             stateStr = "?";         break;
    }
    sprintf(buf, "State:        %s", stateStr);
    PlayInspectorDrawRow(app, row++, buf);

    PlayInspectorFormatTime(fmt1, sizeof(fmt1), position, timescale);
    PlayInspectorFormatTime(fmt2, sizeof(fmt2), duration, timescale);
    sprintf(buf, "Position:     %s / %s", fmt1, fmt2);
    PlayInspectorDrawRow(app, row++, buf);

    sprintf(buf, "TimeScale:    %lu ticks/sec", timescale);
    PlayInspectorDrawRow(app, row++, buf);

    /* blank row */
    PlayInspectorDrawRow(app, row++, "");

    if (sampleRate != 0 || channels != 0) {
        sprintf(buf, "Audio:        %lu Hz x %lu ch",
            sampleRate, channels);
    } else {
        sprintf(buf, "Audio:        n/a");
    }
    PlayInspectorDrawRow(app, row++, buf);

    if (audioBitrate >= 1000UL) {
        sprintf(buf, "Audio kbps:   %lu", audioBitrate / 1000UL);
    } else if (audioBitrate > 0) {
        sprintf(buf, "Audio bps:    %lu", audioBitrate);
    } else {
        sprintf(buf, "Audio kbps:   -");
    }
    PlayInspectorDrawRow(app, row++, buf);

    if (width != 0 || height != 0) {
        sprintf(buf, "Video:        %lu x %lu", width, height);
    } else {
        sprintf(buf, "Video:        n/a (audio-only)");
    }
    PlayInspectorDrawRow(app, row++, buf);

    /* videoFps is Q16.16; show whole.fractional with 2 decimal places. */
    if (videoFps > 0) {
        fpsWhole = (ULONG)videoFps >> 16;
        fpsFrac  = (((ULONG)videoFps & 0xFFFFUL) * 100UL) >> 16;
        sprintf(buf, "Video FPS:    %lu.%02lu  (%lu frames)",
            fpsWhole, fpsFrac, videoFrames);
    } else {
        sprintf(buf, "Video FPS:    -    (%lu frames)", videoFrames);
    }
    PlayInspectorDrawRow(app, row++, buf);

    if (videoBitrate >= 1000UL) {
        sprintf(buf, "Video kbps:   %lu", videoBitrate / 1000UL);
    } else if (videoBitrate > 0) {
        sprintf(buf, "Video bps:    %lu", videoBitrate);
    } else {
        sprintf(buf, "Video kbps:   -");
    }
    PlayInspectorDrawRow(app, row++, buf);

    sprintf(buf, "Dropped:      %lu frames", droppedFrames);
    PlayInspectorDrawRow(app, row++, buf);

    /*
     * Drops per second: compare the deltas of cumulative drops and
     * movie position since the previous refresh. fm_Position runs at
     * fm_TimeScale ticks/sec, so (drops * timescale) / position_delta
     * yields drops/sec without needing wall-clock ticks. We require a
     * stable timescale across the window so a seek that resets the
     * counters does not produce a misleading spike.
     */
    if (PlayInspectorPrevPosition != 0 && position > PlayInspectorPrevPosition &&
        droppedFrames >= PlayInspectorPrevDropped &&
        timescale > 0 && timescale == PlayInspectorPrevTimeScale) {
        dropDelta = droppedFrames - PlayInspectorPrevDropped;
        posDelta = position - PlayInspectorPrevPosition;
        if (posDelta > 0) {
            sprintf(buf, "Drop rate:    %lu / sec",
                (dropDelta * timescale) / posDelta);
            PlayInspectorDrawRow(app, row++, buf);
        }
    }
    PlayInspectorPrevDropped = droppedFrames;
    PlayInspectorPrevPosition = position;
    PlayInspectorPrevTimeScale = timescale;

    if (clockKind == FFCK_REALTIME) {
        PlayInspectorDrawRow(app, row++, "Clock:        REALTIME (Conductor)");
    } else if (clockKind == FFCK_TIMER) {
        PlayInspectorDrawRow(app, row++, "Clock:        TIMER");
    } else if (clockKind == FFCK_AUDIO) {
        PlayInspectorDrawRow(app, row++, "Clock:        AUDIO");
    } else if (clockKind != 0) {
        sprintf(buf, "Clock:        kind %lu", clockKind);
        PlayInspectorDrawRow(app, row++, buf);
    }

    if (width != 0 || height != 0) {
        driftWhole = avDrift / 1000;
        driftAbs = driftWhole;
        if (driftAbs < 0) {
            driftAbs = -driftAbs;
        }
        if (avDrift < 0) {
            driftFrac = (-(avDrift % 1000)) / 100;
        } else {
            driftFrac = (avDrift % 1000) / 100;
        }
        if (avDrift >= 0) {
            sprintf(buf, "A/V drift:    +%ld.%ld ms",
                (LONG)(driftAbs), (LONG)driftFrac);
        } else {
            sprintf(buf, "A/V drift:    -%ld.%ld ms",
                (LONG)(driftAbs), (LONG)driftFrac);
        }
        PlayInspectorDrawRow(app, row++, buf);
    }

    PlayInspectorDrawRow(app, row++, "");
    PlayInspectorDrawRow(app, row++, "Pipeline:");
    PlayInspectorFormatPipelineRow(buf, sizeof(buf), "SRC", src);
    PlayInspectorDrawRow(app, row++, buf);
    PlayInspectorFormatPipelineRow(buf, sizeof(buf), "DMX", demux);
    PlayInspectorDrawRow(app, row++, buf);
    PlayInspectorFormatPipelineRow(buf, sizeof(buf), "COD", codec);
    PlayInspectorDrawRow(app, row++, buf);
    PlayInspectorFormatPipelineRow(buf, sizeof(buf), "ASNK", audioSink);
    PlayInspectorDrawRow(app, row++, buf);
    if (videoSink != NULL) {
        PlayInspectorFormatPipelineRow(buf, sizeof(buf), "VSNK", videoSink);
        PlayInspectorDrawRow(app, row++, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Event handling                                                       */
/* ------------------------------------------------------------------ */
static void
PlayPumpEvents(struct PlayApp *app)
{
    struct FFEvent ev;

    if (app->pa_Movie == NULL) {
        return;
    }
    while (PollFFMovieEvent(app->pa_Movie, &ev) > 0) {
        if (PlayDebug) {
            Printf("DEBUG Play: movie event kind=%ld time=%ld tags=%lx\n",
                ev.fe_Kind, ev.fe_Time, (ULONG)ev.fe_Tags);
        }
        switch (ev.fe_Kind) {
        case FFEV_EOS:
            PlayDrawStatus(app, "End of stream");
            break;
        case FFEV_FORMAT:
        case FFEV_SEEK:
        case FFEV_FLUSH:
        case FFEV_TAGS:
        case FFEV_QOS:
        default:
            break;
        }
    }
}

/*
 * Drain the window's IDCMP port; returns nonzero when the user has
 * asked to quit (close gadget or Project / Quit menu).
 *
 * Menu dispatch uses MENUNUM/ITEMNUM macros from intuition.h: the IDCMP
 * Code field encodes (subnum, itemnum, menunum) and we walk the chain
 * via mi->NextSelect for multi-select picks.
 */
static ULONG
PlayHandleIDCMP(struct PlayApp *app)
{
    struct IntuiMessage *im;
    struct IntuiMessage local;
    ULONG class;
    UWORD code;
    UWORD menuNum;
    UWORD menuId;
    UWORD itemId;
    struct MenuItem *mi;
    ULONG quit;

    quit = 0UL;

    if (app->pa_Window == NULL) {
        return quit;
    }

    while ((im = (struct IntuiMessage *)GetMsg(
            app->pa_Window->UserPort)) != NULL) {

        class = im->Class;
        code  = im->Code;
        /*
         * Cache the fields we need into a stack-local IntuiMessage
         * before ReplyMsg() recycles the original. IAddress in
         * particular is read by PlayTapeDeckHandleEvent() to confirm
         * the GADGETUP came from our tape-deck and not some other
         * gadget Intuition might add later.
         */
        local.Class = im->Class;
        local.Code = im->Code;
        local.Qualifier = im->Qualifier;
        local.IAddress = im->IAddress;
        local.MouseX = im->MouseX;
        local.MouseY = im->MouseY;
        local.Seconds = im->Seconds;
        local.Micros = im->Micros;
        if (PlayDebug) {
            Printf("DEBUG Play: IDCMP class=%lx code=%lx iaddr=%lx\n",
                class, (ULONG)code, (ULONG)im->IAddress);
        }
        ReplyMsg((struct Message *)im);

        switch (class) {
        case IDCMP_CLOSEWINDOW:
            quit = 1UL;
            break;

        case IDCMP_MENUPICK:
            menuNum = code;
            while (menuNum != MENUNULL) {
                mi = ItemAddress(app->pa_Menu, (ULONG)menuNum);
                if (mi == NULL) {
                    break;
                }
                menuId = MENUNUM(menuNum);
                itemId = ITEMNUM(menuNum);
                if (PlayDebug) {
                    Printf("DEBUG Play: menu pick raw=%lx menu=%ld item=%ld next=%lx\n",
                        (ULONG)menuNum, (ULONG)menuId,
                        (ULONG)itemId, (ULONG)mi->NextSelect);
                }

                if (menuId == MENU_PROJECT) {
                    switch (itemId) {
                    case ITEM_OPEN:
                        if (PlayPickFile(app) == RETURN_OK) {
                            PlayOpenMovie(app,
                                (CONST_STRPTR)app->pa_Path);
                        }
                        break;
                    case ITEM_INSPECT:
                        /*
                         * Toggle the Inspector window. Open if not
                         * already up; close it if visible so the same
                         * menu item works as a hide/show toggle (same
                         * UX as Workbench's "Show All" toggle).
                         */
                        if (app->pa_Inspector == NULL) {
                            PlayInspectorOpen(app);
                        } else {
                            PlayInspectorClose(app);
                        }
                        break;
                    case ITEM_QUIT:
                        quit = 1UL;
                        break;
                    default:
                        break;
                    }
                } else if (menuId == MENU_TRANSPORT
                           && app->pa_Movie != NULL) {
                    switch (itemId) {
                    case ITEM_PLAY:
                        PlayFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Playing");
                        break;
                    case ITEM_PAUSE:
                        PauseFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Paused");
                        break;
                    case ITEM_STOP:
                        StopFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Stopped");
                        break;
                    case ITEM_REWIND:
                        RewindFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Rewound");
                        break;
                    case ITEM_FFWD:
                        FastForwardFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Fast forward");
                        break;
                    case ITEM_REVERSE:
                        ReverseFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Reverse");
                        break;
                    default:
                        break;
                    }
                }
                menuNum = mi->NextSelect;
            }
            break;

        case IDCMP_VANILLAKEY:
            if (PlayDebug) {
                Printf("DEBUG Play: vanilla key code=%ld\n",
                    (ULONG)code);
            }
            switch ((UBYTE)code) {
            case ' ':
                if (app->pa_Movie != NULL) {
                    if (GetFFMovieState(app->pa_Movie)
                        == FFMS_PLAYING) {
                        PauseFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Paused");
                        /*
                         * Reflect the new state on the tape-deck so
                         * the play button shows paused. The gadget
                         * does not poll the engine - we have to push
                         * the state to it.
                         */
                        if (app->pa_TapeDeck != NULL) {
                            SetGadgetAttrs(
                                (struct Gadget *)app->pa_TapeDeck,
                                app->pa_Window, NULL,
                                TDECK_Mode,   (ULONG)BUT_PLAY,
                                TDECK_Paused, TRUE,
                                TAG_DONE);
                        }
                    } else {
                        PlayFFMovie(app->pa_Movie);
                        PlayDrawStatus(app, "Playing");
                        if (app->pa_TapeDeck != NULL) {
                            SetGadgetAttrs(
                                (struct Gadget *)app->pa_TapeDeck,
                                app->pa_Window, NULL,
                                TDECK_Mode,   (ULONG)BUT_PLAY,
                                TDECK_Paused, FALSE,
                                TAG_DONE);
                        }
                    }
                }
                break;
            case 27:                /* ESC */
            case 'q':
            case 'Q':
                quit = 1UL;
                break;
            default:
                break;
            }
            break;

        case IDCMP_REFRESHWINDOW:
            BeginRefresh(app->pa_Window);
            EndRefresh(app->pa_Window, TRUE);
            break;

        case IDCMP_NEWSIZE:
            /*
             * The window's Width / Height are now valid for the new
             * box ChangeWindowBox() requested in PlayResizeWindowFor-
             * Video(). PlayResizeWindowForVideo() disposed the
             * bottom-anchored tape-deck before queueing the resize so
             * it could not flash mid-client-area between the resize
             * landing and this handler running; rebuild one here
             * against the now-current geometry and re-seed it with
             * the live movie's state. PlayRebuildTapeDeckAfterResize()
             * is idempotent: if a deck is somehow still attached it
             * is torn down and recreated so its absolute Y always
             * tracks the current Window->Height (matters if a future
             * size-gadget drag, or back-to-back movie opens, fire
             * NEWSIZE with a tape-deck still present). Finally
             * redraw the status row - the row's Y is computed off
             * Window->Height in PlayDrawStatus() so the cached pixels
             * from before the resize sit at the wrong line now.
             */
            if (PlayDebug) {
                Printf("DEBUG Play: NEWSIZE width=%ld height=%ld\n",
                    (LONG)app->pa_Window->Width,
                    (LONG)app->pa_Window->Height);
            }
            PlayRebuildTapeDeckAfterResize(app);
            if (app->pa_Movie == NULL) {
                PlayDrawStatus(app,
                    "Ready - choose Project / Open... or play <file>");
            } else if (PlayNoPlay) {
                PlayDrawStatus(app, (CONST_STRPTR)"Opened");
            } else {
                PlayDrawStatus(app, (CONST_STRPTR)"Playing");
            }
            break;

        case IDCMP_GADGETUP:
            /*
             * The tape-deck gadget posts GADGETUP when a button is
             * clicked or the slider is dropped. PlayTapeDeckHandle-
             * Event() checks IAddress against our gadget and ignores
             * unrelated GADGETUPs (e.g. if a future revision adds
             * more gadgets to the window).
             */
            PlayTapeDeckHandleEvent(app, &local);
            break;

        case IDCMP_MOUSEMOVE:
            /*
             * MOUSEMOVE only fires while a GA_FollowMouse gadget is
             * active. Forward the latest slider value to the engine
             * so the user can scrub by ear.
             */
            PlayTapeDeckHandleScrub(app);
            break;

        case IDCMP_INTUITICKS:
            /*
             * ~10 Hz tick whenever the mouse is over the window.
             * Cheap chance to keep the slider in sync with the
             * playback head. The movie's own signal also kicks this
             * update via PlayMainLoop().
             */
            PlayTapeDeckRefreshFromMovie(app);
            break;

        default:
            break;
        }

        if (quit != 0UL) {
            break;
        }
    }
    return quit;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                            */
/* ------------------------------------------------------------------ */
static LONG
PlayMainLoop(struct PlayApp *app)
{
    ULONG winSig;
    ULONG movieSig;
    ULONG ctrlc;
    ULONG signals;
    ULONG quit;
    ULONG state;

    ctrlc = SIGBREAKF_CTRL_C;
    quit = 0UL;

    if (app->pa_Window == NULL) {
        return RETURN_FAIL;
    }
    winSig = 1UL << app->pa_Window->UserPort->mp_SigBit;
    if (PlayDebug) {
        Printf("DEBUG Play: main loop start winSig=%lx ctrlc=%lx\n",
            winSig, ctrlc);
    }

    SetSignal(0, ctrlc);

    while (quit == 0UL) {
        movieSig = 0UL;
        if (app->pa_Movie != NULL) {
            movieSig = GetFFMovieSignalMask(app->pa_Movie);
        }

        if (PlayDebug) {
            Printf("DEBUG Play: waiting winSig=%lx movieSig=%lx"
                " inspectSig=%lx ctrlc=%lx\n",
                winSig, movieSig, app->pa_InspectorSigMask, ctrlc);
        }
        /*
         * Inspector's signal mask is OR'd in only when the inspector
         * window is currently open (pa_InspectorSigMask is 0
         * otherwise). This is the simplest way to add an optional
         * second window without restructuring the wait loop.
         */
        signals = Wait(winSig | movieSig | app->pa_InspectorSigMask | ctrlc);
        if (PlayDebug) {
            Printf("DEBUG Play: woke signals=%lx\n", signals);
        }

        if ((signals & ctrlc) != 0UL) {
            PlayLogWarn("***Break (CTRL-C)");
            if (PlayDebug) {
                Printf("DEBUG Play: CTRL-C received\n");
            }
            if (app->pa_Movie != NULL) {
                StopFFMovie(app->pa_Movie);
            }
            quit = 1UL;
            break;
        }

        if ((signals & winSig) != 0UL) {
            if (PlayDebug) {
                Printf("DEBUG Play: handling window signal\n");
            }
            quit = PlayHandleIDCMP(app);
        }

        /*
         * Inspector window events. Closing the inspector does NOT
         * quit the app (it is a secondary window), so we discard the
         * handler's return value here - PlayInspectorHandleIDCMP()
         * closes its own window on IDCMP_CLOSEWINDOW. The check
         * against pa_InspectorSigMask prevents servicing a stale
         * signal bit if the window was closed under us by some other
         * path between Wait() and now.
         */
        if (app->pa_InspectorSigMask != 0UL &&
            (signals & app->pa_InspectorSigMask) != 0UL) {
            PlayInspectorHandleIDCMP(app);
        }

        if ((signals & movieSig) != 0UL || app->pa_Movie != NULL) {
            PlayPumpEvents(app);
            if (app->pa_Movie != NULL) {
                state = GetFFMovieState(app->pa_Movie);
                if (PlayDebug) {
                    Printf("DEBUG Play: movie state=%ld\n", state);
                }
                if (state == FFMS_FINISHED) {
                    PlayDrawStatus(app, "Finished");
                }
                /*
                 * Refresh the tape-deck slider on every wake-up so
                 * the user sees the play head advance as audio
                 * plays. The function is a cheap GetAttr +
                 * SetGadgetAttrs and is throttled internally via
                 * pa_LastSliderValue: it only invokes Intuition when
                 * the slider value would actually change.
                 */
                PlayTapeDeckRefreshFromMovie(app);
                /*
                 * Inspector window mirrors the same cadence: redraw
                 * its stats on every movie wake-up so FPS / bitrate
                 * track the engine in (near) real time. The function
                 * silently no-ops when the inspector window is closed,
                 * so this costs nothing in the common case.
                 */
                PlayInspectorRefresh(app);
            }
        }
    }
    return RETURN_OK;
}

/* ------------------------------------------------------------------ */
/* Top-level entry helpers                                              */
/* ------------------------------------------------------------------ */
static LONG
PlayFromPath(CONST_STRPTR path)
{
    struct PlayApp app;
    LONG rc;

    app.pa_Window = NULL;
    app.pa_Menu   = NULL;
    app.pa_Movie  = NULL;
    app.pa_TapeDeck = NULL;
    app.pa_LastSliderValue = 0L;
    app.pa_Inspector = NULL;
    app.pa_InspectorSigMask = 0UL;
    app.pa_Path[0]   = '\0';
    app.pa_Drawer[0] = '\0';
    app.pa_File[0]   = '\0';

    if (PlayDebug) {
        Printf("DEBUG Play: PlayFromPath path='%s'\n",
            path != NULL ? path : (STRPTR)"");
    }

    rc = PlayOpenLibs();
    if (rc != RETURN_OK) {
        if (PlayDebug) {
            Printf("DEBUG Play: PlayOpenLibs failed rc=%ld\n", rc);
        }
        PlayCloseLibs();
        return rc;
    }

    rc = PlayOpenWindow(&app);
    if (rc != RETURN_OK) {
        if (PlayDebug) {
            Printf("DEBUG Play: PlayOpenWindow failed rc=%ld\n", rc);
        }
        PlayCloseLibs();
        return rc;
    }

    /*
     * STATS=STATISTICS/S (CLI) or ToolType STATS (Workbench): open the
     * inspector before the first movie so counters are visible from the
     * first frame of playback.
     */
    if (PlayStartWithInspector != FALSE) {
        PlayInspectorOpen(&app);
    }

    if (path != NULL) {
        FFStrCopyN(app.pa_Path, (STRPTR)path, sizeof(app.pa_Path));
        rc = PlayOpenMovie(&app, (CONST_STRPTR)app.pa_Path);
        if (rc != RETURN_OK) {
            /* keep the window open so the user can pick another file */
            rc = RETURN_OK;
        }
    } else {
        if (PlayDebug) {
            Printf("DEBUG Play: no path supplied; using requester\n");
        }
        if (PlayPickFile(&app) == RETURN_OK) {
            PlayOpenMovie(&app, (CONST_STRPTR)app.pa_Path);
        }
    }

    rc = PlayMainLoop(&app);

    PlayLogInfo("done");
    if (PlayDebug) {
        Printf("DEBUG Play: PlayFromPath returning rc=%ld\n", rc);
    }
    PlayCloseMovie(&app);
    PlayCloseWindow(&app);
    PlayCloseLibs();

    return rc;
}

static LONG
PlayFromWB(struct WBStartup *wb)
{
    struct WBArg *arg;
    BPTR olddir;
    LONG changedir;
    LONG rc;

    rc = RETURN_FAIL;
    olddir = 0;
    changedir = FALSE;

    if (wb == NULL) {
        return rc;
    }

    arg = wb->sm_ArgList;
    if (PlayDebug) {
        Printf("DEBUG Play: Workbench startup numargs=%ld arglist=%lx\n",
            (ULONG)wb->sm_NumArgs, (ULONG)arg);
    }
    if (arg == NULL || wb->sm_NumArgs < 1) {
        return rc;
    }

    /*
     * arg[0] is the tool itself. arg[1..] are project icons that were
     * dropped on the tool. If the user just double-clicked the tool we
     * still launch and fall back to the ASL requester for path entry.
     */
    if (wb->sm_NumArgs >= 2) {
        if (PlayDebug) {
            Printf("DEBUG Play: Workbench project lock=%lx name='%s'\n",
                (ULONG)arg[1].wa_Lock,
                arg[1].wa_Name != NULL ? arg[1].wa_Name : (STRPTR)"");
        }
        if (arg[1].wa_Lock != 0) {
            olddir = CurrentDir(arg[1].wa_Lock);
            changedir = TRUE;
        }
        rc = PlayFromPath((CONST_STRPTR)arg[1].wa_Name);
        if (changedir) {
            CurrentDir(olddir);
        }
    } else {
        rc = PlayFromPath(NULL);
    }
    return rc;
}

int
main(int argc, char **argv)
{
    struct RDArgs *rdargs;
    LONG rargs[4];
    UBYTE cliPath[256];
    STRPTR path;
    LONG rc;

    rdargs = NULL;
    rargs[0] = 0;
    rargs[1] = 0;
    rargs[2] = 0;
    rargs[3] = 0;
    cliPath[0] = '\0';
    path = NULL;
    rc = RETURN_FAIL;

    /*
     * Workbench launch: ac is 0 and av is really a (struct WBStartup *).
     */
    if (argc == 0) {
        rc = PlayFromWB((struct WBStartup *)argv);
        return (int)rc;
    }

    /*
     * Standard CLI lifecycle (per DTConvert / AMP):
     *   - DOSBase is provided by the SAS/C C-runtime startup, no manual
     *     OpenLibrary("dos.library") needed.
     *   - ReadArgs() with NULL rdargs reads the CLI command line that
     *     the shell handed us.
     *   - rargs[] is cleared first so unset options stay 0 / NULL.
     *
     * Template:
     *   FILE      - optional positional, media path (STRPTR via rargs[0])
     *   DEBUG/S   - boolean switch, enables verbose runtime trace
     *               (0 / non-zero via rargs[1])
     *   NOPLAY/S  - boolean switch, opens movie but skips autoplay so
     *               crashes can be isolated to the open vs. run path
     *               (0 / non-zero via rargs[2])
     *   STATS=STATISTICS/S - boolean switch, opens the Inspector window
     *               on startup (0 / non-zero via rargs[3])
     */
    rdargs = ReadArgs((STRPTR)"FILE,DEBUG/S,NOPLAY/S,STATS=STATISTICS/S",
        rargs, NULL);
    if (rdargs == NULL) {
        PrintFault(IoErr(), (STRPTR)"Play");
        return RETURN_FAIL;
    }

    if (rargs[1] != 0) {
        PlayDebug = TRUE;
    }
    if (rargs[2] != 0) {
        PlayNoPlay = TRUE;
    }
    if (rargs[3] != 0) {
        PlayStartWithInspector = TRUE;
    }
    if (rargs[0] != 0) {
        FFStrCopyN((STRPTR)cliPath, (STRPTR)rargs[0],
            sizeof(cliPath));
        path = (STRPTR)cliPath;
    }

    if (PlayDebug) {
        Printf("DEBUG Play: ReadArgs template"
            " FILE,DEBUG/S,NOPLAY/S,STATS=STATISTICS/S\n");
        Printf("DEBUG Play: argc=%ld file='%s' debug=%ld noplay=%ld"
            " stats=%ld\n",
            (ULONG)argc, path != NULL ? path : (STRPTR)"",
            (ULONG)PlayDebug, (ULONG)PlayNoPlay,
            (ULONG)PlayStartWithInspector);
    }

    FreeArgs(rdargs);

    rc = PlayFromPath((CONST_STRPTR)path);
    return (int)rc;
}
