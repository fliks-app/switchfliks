#pragma once
#include "gfx/Color.h"

// The values here are the Fliks web client's, read off `client/src/styles.css`
// and daisyUI 5's `dark` theme, converted from oklch to sRGB. The whole UI is
// laid out in "CSS pixels" on a 960x540 canvas, which is what the TV build
// actually lays out in: a 1080p TV reports innerWidth=960 at DPR=2, so a
// docked Switch reproduces the 10-foot sizing 1:1 and handheld scales down.
namespace theme {

// daisyUI `dark`
constexpr gfx::Color Base100        = gfx::Color::rgb(0x1d232a);
constexpr gfx::Color Base200        = gfx::Color::rgb(0x191e24);
constexpr gfx::Color Base300        = gfx::Color::rgb(0x15191e);
constexpr gfx::Color BaseContent    = gfx::Color::rgb(0xecf9ff);
constexpr gfx::Color Primary        = gfx::Color::rgb(0x605dff);
constexpr gfx::Color PrimaryContent = gfx::Color::rgb(0xedf1fe);
constexpr gfx::Color Secondary      = gfx::Color::rgb(0xf43098);
constexpr gfx::Color Accent         = gfx::Color::rgb(0x00d3bb);
constexpr gfx::Color Neutral        = gfx::Color::rgb(0x09090b);
constexpr gfx::Color Info           = gfx::Color::rgb(0x00bafe);
constexpr gfx::Color Success        = gfx::Color::rgb(0x00d390);
constexpr gfx::Color SuccessContent = gfx::Color::rgb(0x004c39);
constexpr gfx::Color Warning        = gfx::Color::rgb(0xfcb700);
constexpr gfx::Color Error          = gfx::Color::rgb(0xff627d);
constexpr gfx::Color White          = gfx::Color::rgb(0xffffff);
constexpr gfx::Color Black          = gfx::Color::rgb(0x000000);

// `.app-bg-veil` — the near-opaque tint the fanart sits behind.
constexpr gfx::Color BgVeil         = gfx::Color::rgba(0x1d232a, 0.95f);
// `body.tv .app-sidebar-veil`
constexpr gfx::Color SidebarVeil    = gfx::Color::rgba(0x1d232a, 0.55f);

namespace metric {

constexpr float CanvasW = 960.0f;
constexpr float CanvasH = 540.0f;

// `body.tv { font-size: 18px; --page-p: 4vw }`
constexpr float BodyFont   = 18.0f;
constexpr float PagePad    = 38.0f;

// daisyUI radii as re-asserted for TV in styles.css
constexpr float RadiusBox      = 8.0f;   // --radius-box .5rem, rounded-lg
constexpr float RadiusField    = 16.0f;  // --radius-field 1rem
constexpr float RadiusSelector = 16.0f;

// Card focus: `0 0 0 2px base-100, 0 0 0 6px primary`, blurs dropped on TV.
constexpr float FocusGap  = 2.0f;
constexpr float FocusRing = 6.0f;

// `body.tv-androidtv` card footprint, the densest of the TV ladders.
constexpr float CardPortraitW   = 120.0f;
constexpr float CardPortraitH   = 180.0f;  // aspect-2/3
constexpr float CardLandscapeW  = 208.0f;
constexpr float CardLandscapeH  = 117.0f;  // aspect-video
constexpr float LibraryPillW    = 160.0f;
constexpr float LibraryPillH    = 96.0f;

// `body.tv app-media-card h3 / p`
constexpr float CardTitleFont = 20.0f;
constexpr float CardSubFont   = 15.2f;
constexpr float CardCaptionGap = 6.0f;   // pt-1.5

// horizontal-scroller: `tv:gap-6`, title `text-lg font-bold` + `mb-3`,
// rails separated by the home page's `gap-8`.
constexpr float RailGap       = 24.0f;
constexpr float RailTitleFont = 18.0f;
constexpr float RailTitleGap  = 12.0f;
constexpr float RailSpacing   = 32.0f;

// Layout chrome
constexpr float SidebarW      = 256.0f;
constexpr float TopBarH       = 56.0f;
constexpr float HintBarH      = 40.0f;

} // namespace metric
} // namespace theme
