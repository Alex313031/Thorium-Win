// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file defines utility functions for X11 (Linux only). This code has been
// ported from XCB since we can't use XCB on Ubuntu while its 32-bit support
// remains woefully incomplete.

#include "ui/base/x/x11_util.h"

#include <ctype.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <bitset>
#include <limits>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/contains.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/singleton.h"
#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/trace_event/trace_event.h"
#include "base/values.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/x/visual_picker_glx.h"
#include "ui/display/util/gpu_info_util.h"
#include "ui/events/devices/x11/device_data_manager_x11.h"
#include "ui/events/devices/x11/touch_factory_x11.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/switches.h"
#include "ui/gfx/x/connection.h"
#include "ui/gfx/x/screensaver.h"
#include "ui/gfx/x/shm.h"
#include "ui/gfx/x/x11_atom_cache.h"
#include "ui/gfx/x/xproto.h"
#include "ui/gfx/x/xproto_util.h"

#if defined(OS_FREEBSD)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace ui {
namespace {

// Constants that are part of EWMH.
constexpr int kNetWMStateAdd = 1;
constexpr int kNetWMStateRemove = 0;

bool SupportsEWMH() {
  static bool supports_ewmh = false;
  static bool supports_ewmh_cached = false;
  if (!supports_ewmh_cached) {
    supports_ewmh_cached = true;

    x11::Window wm_window = x11::Window::None;
    if (!GetProperty(GetX11RootWindow(),
                     x11::GetAtom("_NET_SUPPORTING_WM_CHECK"), &wm_window)) {
      supports_ewmh = false;
      return false;
    }

    // It's possible that a window manager started earlier in this X session
    // left a stale _NET_SUPPORTING_WM_CHECK property when it was replaced by a
    // non-EWMH window manager, so we trap errors in the following requests to
    // avoid crashes (issue 23860).

    // EWMH requires the supporting-WM window to also have a
    // _NET_SUPPORTING_WM_CHECK property pointing to itself (to avoid a stale
    // property referencing an ID that's been recycled for another window), so
    // we check that too.
    x11::Window wm_window_property = x11::Window::None;
    supports_ewmh =
        GetProperty(wm_window, x11::GetAtom("_NET_SUPPORTING_WM_CHECK"),
                    &wm_window_property) &&
        wm_window_property == wm_window;
  }

  return supports_ewmh;
}

bool GetWindowManagerName(std::string* wm_name) {
  DCHECK(wm_name);
  if (!SupportsEWMH())
    return false;

  x11::Window wm_window = x11::Window::None;
  if (!GetProperty(GetX11RootWindow(), x11::GetAtom("_NET_SUPPORTING_WM_CHECK"),
                   &wm_window)) {
    return false;
  }

  std::vector<char> str;
  if (!GetArrayProperty(wm_window, x11::GetAtom("_NET_WM_NAME"), &str))
    return false;
  wm_name->assign(str.data(), str.size());
  return true;
}

// Returns whether the X11 Screen Saver Extension can be used to disable the
// screen saver.
bool IsX11ScreenSaverAvailable() {
  // X Screen Saver isn't accessible in headless mode.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless))
    return false;

  auto version = x11::Connection::Get()
                     ->screensaver()
                     .QueryVersion({x11::ScreenSaver::major_version,
                                    x11::ScreenSaver::minor_version})
                     .Sync();

  return version && (version->server_major_version > 1 ||
                     (version->server_major_version == 1 &&
                      version->server_minor_version >= 1));
}

// Returns the bounds of |window| in the screen before adjusting for the frame.
bool GetUndecoratedWindowBounds(x11::Window window, gfx::Rect* rect) {
  auto root = GetX11RootWindow();

  x11::Connection* connection = x11::Connection::Get();
  auto get_geometry = connection->GetGeometry(window);
  auto translate_coords = connection->TranslateCoordinates({window, root});

  // Sync after making both requests so only one round-trip is made.
  // Flush so all requests are sent before waiting on any replies.
  connection->Flush();
  auto geometry = get_geometry.Sync();
  auto coords = translate_coords.Sync();

  if (!geometry || !coords)
    return false;

  *rect = gfx::Rect(coords->dst_x, coords->dst_y, geometry->width,
                    geometry->height);
  return true;
}

// Obtains the value of _{NET,GTK}_FRAME_EXTENTS as a gfx::Insets.  Returns an
// empty gfx::Insets if the property doesn't exist or is malformed.
gfx::Insets GetFrameExtentsProperty(x11::Window window, x11::Atom property) {
  std::vector<int32_t> frame_extents;
  GetArrayProperty(window, property, &frame_extents);
  if (frame_extents.size() != 4)
    return gfx::Insets();
  return gfx::Insets(frame_extents[2] /* top */, frame_extents[0] /* left */,
                     frame_extents[3] /* bottom */,
                     frame_extents[1] /* right */);
}

// Returns the adjustment necessary to obtain the opaque bounds of |window|.
gfx::Insets GetWindowDecorationAdjustment(x11::Window window) {
  // _GTK_FRAME_EXTENTS is set by clients using client side decorations to
  // subtract the window shadow from the bounds.  _NET_FRAME_EXTENTS is set by
  // the WM to add the opaque portion of the frame to the bounds.
  return GetFrameExtentsProperty(window, x11::GetAtom("_GTK_FRAME_EXTENTS")) -
         GetFrameExtentsProperty(window, x11::GetAtom("_NET_FRAME_EXTENTS"));
}

// Returns the opaque bounds of |window| with it's frame.
bool GetDecoratedWindowBounds(x11::Window window, gfx::Rect* rect) {
  if (!GetUndecoratedWindowBounds(window, rect))
    return false;

  rect->Inset(GetWindowDecorationAdjustment(window));
  return true;
}

// Returns true if the event has event_x and event_y fields.
bool EventHasCoordinates(const x11::Event& event) {
  return event.As<x11::KeyEvent>() || event.As<x11::ButtonEvent>() ||
         event.As<x11::MotionNotifyEvent>() || event.As<x11::CrossingEvent>() ||
         event.As<x11::Input::LegacyDeviceEvent>() ||
         event.As<x11::Input::DeviceEvent>() ||
         event.As<x11::Input::CrossingEvent>();
}

}  // namespace

bool GetWmNormalHints(x11::Window window, SizeHints* hints) {
  std::vector<uint32_t> hints32;
  if (!GetArrayProperty(window, x11::Atom::WM_NORMAL_HINTS, &hints32))
    return false;
  if (hints32.size() != sizeof(SizeHints) / 4)
    return false;
  memcpy(hints, hints32.data(), sizeof(*hints));
  return true;
}

void SetWmNormalHints(x11::Window window, const SizeHints& hints) {
  std::vector<uint32_t> hints32(sizeof(SizeHints) / 4);
  memcpy(hints32.data(), &hints, sizeof(SizeHints));
  SetArrayProperty(window, x11::Atom::WM_NORMAL_HINTS, x11::Atom::WM_SIZE_HINTS,
                   hints32);
}

bool GetWmHints(x11::Window window, WmHints* hints) {
  std::vector<uint32_t> hints32;
  if (!GetArrayProperty(window, x11::Atom::WM_HINTS, &hints32))
    return false;
  if (hints32.size() != sizeof(WmHints) / 4)
    return false;
  memcpy(hints, hints32.data(), sizeof(*hints));
  return true;
}

void SetWmHints(x11::Window window, const WmHints& hints) {
  std::vector<uint32_t> hints32(sizeof(WmHints) / 4);
  memcpy(hints32.data(), &hints, sizeof(WmHints));
  SetArrayProperty(window, x11::Atom::WM_HINTS, x11::Atom::WM_HINTS, hints32);
}

void WithdrawWindow(x11::Window window) {
  auto* connection = x11::Connection::Get();
  connection->UnmapWindow({window});

  auto root = connection->default_root();
  x11::UnmapNotifyEvent event{.event = root, .window = window};
  auto mask =
      x11::EventMask::SubstructureNotify | x11::EventMask::SubstructureRedirect;
  SendEvent(event, root, mask);
}

void RaiseWindow(x11::Window window) {
  x11::Connection::Get()->ConfigureWindow(x11::ConfigureWindowRequest{
      .window = window, .stack_mode = x11::StackMode::Above});
}

void LowerWindow(x11::Window window) {
  x11::Connection::Get()->ConfigureWindow(x11::ConfigureWindowRequest{
      .window = window, .stack_mode = x11::StackMode::Below});
}

void DefineCursor(x11::Window window, x11::Cursor cursor) {
  // TODO(https://crbug.com/1066670): Sync() should be removed.  It's added for
  // now because Xlib's XDefineCursor() sync'ed and removing it perturbs the
  // timing on BookmarkBarViewTest8.DNDBackToOriginatingMenu on
  // linux-chromeos-rel, causing it to flake.
  x11::Connection::Get()
      ->ChangeWindowAttributes(x11::ChangeWindowAttributesRequest{
          .window = window, .cursor = cursor})
      .Sync();
}

size_t RowBytesForVisualWidth(const x11::Connection::VisualInfo& visual_info,
                              int width) {
  auto bpp = visual_info.format->bits_per_pixel;
  auto align = visual_info.format->scanline_pad;
  size_t row_bits = bpp * width;
  row_bits += (align - (row_bits % align)) % align;
  return (row_bits + 7) / 8;
}

void DrawPixmap(x11::Connection* connection,
                x11::VisualId visual,
                x11::Drawable drawable,
                x11::GraphicsContext gc,
                const SkPixmap& skia_pixmap,
                int src_x,
                int src_y,
                int dst_x,
                int dst_y,
                int width,
                int height) {
  // 24 bytes for the PutImage header, an additional 4 bytes in case this is an
  // extended size request, and an additional 4 bytes in case padding is needed.
  constexpr size_t kPutImageExtraSize = 32;

  const auto* visual_info = connection->GetVisualInfoFromId(visual);
  if (!visual_info)
    return;

  size_t row_bytes = RowBytesForVisualWidth(*visual_info, width);

  auto color_type = ColorTypeForVisual(visual);
  if (color_type == kUnknown_SkColorType) {
    // TODO(https://crbug.com/1066670): Add a fallback path in case any users
    // are running a server that uses visual types for which Skia doesn't have
    // a corresponding color format.
    return;
  }
  SkImageInfo image_info =
      SkImageInfo::Make(width, height, color_type, kPremul_SkAlphaType);

  std::vector<uint8_t> vec(row_bytes * height);
  SkPixmap pixmap(image_info, vec.data(), row_bytes);
  skia_pixmap.readPixels(pixmap, src_x, src_y);

  DCHECK_GT(connection->MaxRequestSizeInBytes(), kPutImageExtraSize);
  int rows_per_request =
      (connection->MaxRequestSizeInBytes() - kPutImageExtraSize) / row_bytes;
  DCHECK_GT(rows_per_request, 1);
  for (int row = 0; row < height; row += rows_per_request) {
    size_t n_rows = std::min<size_t>(rows_per_request, height - row);
    auto data = base::MakeRefCounted<base::RefCountedStaticMemory>(
        vec.data() + row * row_bytes, n_rows * row_bytes);
    connection->PutImage({
        .format = x11::ImageFormat::ZPixmap,
        .drawable = drawable,
        .gc = gc,
        .width = static_cast<uint16_t>(width),
        .height = static_cast<uint16_t>(n_rows),
        .dst_x = static_cast<int16_t>(dst_x),
        .dst_y = static_cast<int16_t>(dst_y + row),
        .left_pad = 0,
        .depth = visual_info->format->depth,
        .data = data,
    });
  }
  // Flush since the PutImage requests depend on |vec| being alive.
  connection->Flush();
}

bool IsXInput2Available() {
  return DeviceDataManagerX11::GetInstance()->IsXInput2Available();
}

bool QueryShmSupport() {
  static bool supported = x11::Connection::Get()->shm().QueryVersion().Sync();
  return supported;
}

int CoalescePendingMotionEvents(const x11::Event& x11_event,
                                x11::Event* last_event) {
  auto* conn = x11::Connection::Get();
  auto* ddmx11 = ui::DeviceDataManagerX11::GetInstance();
  int num_coalesced = 0;

  const auto* motion = x11_event.As<x11::MotionNotifyEvent>();
  const auto* device = x11_event.As<x11::Input::DeviceEvent>();
  DCHECK(motion || device);
  DCHECK(!device || device->opcode == x11::Input::DeviceEvent::Motion ||
         device->opcode == x11::Input::DeviceEvent::TouchUpdate);

  conn->ReadResponses();
  for (auto& event : conn->events()) {
    // There may be non-input events such as ConfigureNotifyEvents and
    // PropertyNotifyEvents that get interleaved between mouse events, so it is
    // necessary to skip over those to coalesce as many pending motion events as
    // possible so mouse dragging is smooth.
    if (!EventHasCoordinates(event))
      continue;

    if (motion) {
      const auto* next_motion = event.As<x11::MotionNotifyEvent>();

      // Discard all but the most recent motion event that targets the same
      // window with unchanged state.
      if (next_motion && next_motion->event == motion->event &&
          next_motion->child == motion->child &&
          next_motion->state == motion->state) {
        *last_event = std::move(event);
        continue;
      }
    } else {
      auto* next_device = event.As<x11::Input::DeviceEvent>();
      if (!next_device)
        break;

      // If this isn't from a valid device, throw the event away, as
      // that's what the message pump would do. Device events come in pairs
      // with one from the master and one from the slave so there will
      // always be at least one pending.
      if (!ui::TouchFactory::GetInstance()->ShouldProcessDeviceEvent(
              *next_device)) {
        event = x11::Event();
        continue;
      }

      // Confirm that the motion event is of the same type, is
      // targeted at the same window, and that no buttons or modifiers
      // have changed.
      if (next_device->opcode == device->opcode &&
          !ddmx11->IsCMTGestureEvent(event) &&
          ddmx11->GetScrollClassEventDetail(event) == SCROLL_TYPE_NO_SCROLL &&
          device->event == next_device->event &&
          device->child == next_device->child &&
          device->detail == next_device->detail &&
          device->button_mask == next_device->button_mask &&
          device->mods.base == next_device->mods.base &&
          device->mods.latched == next_device->mods.latched &&
          device->mods.locked == next_device->mods.locked &&
          device->mods.effective == next_device->mods.effective) {
        *last_event = std::move(event);
        num_coalesced++;
        continue;
      }
    }
    break;
  }

  return num_coalesced;
}

void SetUseOSWindowFrame(x11::Window window, bool use_os_window_frame) {
  // This data structure represents additional hints that we send to the window
  // manager and has a direct lineage back to Motif, which defined this de facto
  // standard. We define this struct to match the wire-format (32-bit fields)
  // rather than the Xlib API (XChangeProperty) format (long fields).
  typedef struct {
    uint32_t flags;
    uint32_t functions;
    uint32_t decorations;
    int32_t input_mode;
    uint32_t status;
  } MotifWmHints;

  MotifWmHints motif_hints;
  memset(&motif_hints, 0, sizeof(motif_hints));
  // Signals that the reader of the _MOTIF_WM_HINTS property should pay
  // attention to the value of |decorations|.
  motif_hints.flags = (1u << 1);
  motif_hints.decorations = use_os_window_frame ? 1 : 0;

  std::vector<uint32_t> hints(sizeof(MotifWmHints) / sizeof(uint32_t));
  memcpy(hints.data(), &motif_hints, sizeof(MotifWmHints));
  x11::Atom hint_atom = x11::GetAtom("_MOTIF_WM_HINTS");
  SetArrayProperty(window, hint_atom, hint_atom, hints);
}

bool IsShapeExtensionAvailable() {
  return x11::Connection::Get()->shape().present();
}

x11::Window GetX11RootWindow() {
  return x11::Connection::Get()->default_screen().root;
}

bool GetCurrentDesktop(int32_t* desktop) {
  return GetProperty(GetX11RootWindow(), x11::GetAtom("_NET_CURRENT_DESKTOP"),
                     desktop);
}

void SetHideTitlebarWhenMaximizedProperty(x11::Window window,
                                          HideTitlebarWhenMaximized property) {
  SetProperty(window, x11::GetAtom("_GTK_HIDE_TITLEBAR_WHEN_MAXIMIZED"),
              x11::Atom::CARDINAL, static_cast<uint32_t>(property));
}

bool IsWindowVisible(x11::Window window) {
  TRACE_EVENT0("ui", "IsWindowVisible");

  auto response = x11::Connection::Get()->GetWindowAttributes({window}).Sync();
  if (!response || response->map_state != x11::MapState::Viewable)
    return false;

  // Minimized windows are not visible.
  std::vector<x11::Atom> wm_states;
  if (GetArrayProperty(window, x11::GetAtom("_NET_WM_STATE"), &wm_states)) {
    x11::Atom hidden_atom = x11::GetAtom("_NET_WM_STATE_HIDDEN");
    if (base::Contains(wm_states, hidden_atom))
      return false;
  }

  // Do not check _NET_CURRENT_DESKTOP/_NET_WM_DESKTOP since some
  // window managers (eg. i3) have per-monitor workspaces where more
  // than one workspace can be visible at once, but only one will be
  // "active".
  return true;
}

bool WindowContainsPoint(x11::Window window, gfx::Point screen_loc) {
  TRACE_EVENT0("ui", "WindowContainsPoint");

  gfx::Rect undecorated_bounds;
  if (!GetUndecoratedWindowBounds(window, &undecorated_bounds))
    return false;

  gfx::Rect decorated_bounds = undecorated_bounds;
  decorated_bounds.Inset(GetWindowDecorationAdjustment(window));
  if (!decorated_bounds.Contains(screen_loc))
    return false;

  if (!IsShapeExtensionAvailable())
    return true;

  // According to http://www.x.org/releases/X11R7.6/doc/libXext/shapelib.html,
  // if an X display supports the shape extension the bounds of a window are
  // defined as the intersection of the window bounds and the interior
  // rectangles. This means to determine if a point is inside a window for the
  // purpose of input handling we have to check the rectangles in the ShapeInput
  // list.
  // According to http://www.x.org/releases/current/doc/xextproto/shape.html,
  // we need to also respect the ShapeBounding rectangles.
  // The effective input region of a window is defined to be the intersection
  // of the client input region with both the default input region and the
  // client bounding region. Any portion of the client input region that is not
  // included in both the default input region and the client bounding region
  // will not be included in the effective input region on the screen.
  x11::Shape::Sk rectangle_kind[] = {x11::Shape::Sk::Input,
                                     x11::Shape::Sk::Bounding};
  for (auto kind : rectangle_kind) {
    auto shape =
        x11::Connection::Get()->shape().GetRectangles({window, kind}).Sync();
    if (!shape)
      return true;
    if (shape->rectangles.empty()) {
      // The shape can be empty when |window| is minimized.
      return false;
    }
    bool is_in_shape_rects = false;
    for (const auto& rect : shape->rectangles) {
      // The ShapeInput and ShapeBounding rects are to be in window space, so we
      // have to translate by the window_rect's offset to map to screen space.
      gfx::Rect shape_rect =
          gfx::Rect(rect.x + undecorated_bounds.x(),
                    rect.y + undecorated_bounds.y(), rect.width, rect.height);
      if (shape_rect.Contains(screen_loc)) {
        is_in_shape_rects = true;
        break;
      }
    }
    if (!is_in_shape_rects)
      return false;
  }
  return true;
}

bool PropertyExists(x11::Window window, x11::Atom property) {
  auto response = x11::Connection::Get()
                      ->GetProperty(x11::GetPropertyRequest{
                          .window = window,
                          .property = property,
                          .long_length = 1,
                      })
                      .Sync();
  return response && response->format;
}

bool GetRawBytesOfProperty(x11::Window window,
                           x11::Atom property,
                           scoped_refptr<base::RefCountedMemory>* out_data,
                           x11::Atom* out_type) {
  auto future = x11::Connection::Get()->GetProperty(x11::GetPropertyRequest{
      .window = window,
      .property = property,
      // Don't limit the amount of returned data.
      .long_length = std::numeric_limits<uint32_t>::max(),
  });
  auto response = future.Sync();
  if (!response || !response->format)
    return false;
  *out_data = response->value;
  if (out_type)
    *out_type = response->type;
  return true;
}

void SetWindowClassHint(x11::Connection* connection,
                        x11::Window window,
                        const std::string& res_name,
                        const std::string& res_class) {
  auto str =
      base::StringPrintf("%s%c%s", res_name.c_str(), '\0', res_class.c_str());
  std::vector<char> data(str.data(), str.data() + str.size() + 1);
  SetArrayProperty(window, x11::Atom::WM_CLASS, x11::Atom::STRING, data);
}

void SetWindowRole(x11::Window window, const std::string& role) {
  x11::Atom prop = x11::GetAtom("WM_WINDOW_ROLE");
  if (role.empty())
    x11::DeleteProperty(window, prop);
  else
    x11::SetStringProperty(window, prop, x11::Atom::STRING, role);
}

void SetWMSpecState(x11::Window window,
                    bool enabled,
                    x11::Atom state1,
                    x11::Atom state2) {
  SendClientMessage(
      window, GetX11RootWindow(), x11::GetAtom("_NET_WM_STATE"),
      {static_cast<uint32_t>(enabled ? kNetWMStateAdd : kNetWMStateRemove),
       static_cast<uint32_t>(state1), static_cast<uint32_t>(state2), 1, 0});
}

void DoWMMoveResize(x11::Connection* connection,
                    x11::Window root_window,
                    x11::Window window,
                    const gfx::Point& location_px,
                    int direction) {
  // This handler is usually sent when the window has the implicit grab.  We
  // need to dump it because what we're about to do is tell the window manager
  // that it's now responsible for moving the window around; it immediately
  // grabs when it receives the event below.
  connection->UngrabPointer({x11::Time::CurrentTime});

  SendClientMessage(window, root_window, x11::GetAtom("_NET_WM_MOVERESIZE"),
                    {static_cast<uint32_t>(location_px.x()),
                     static_cast<uint32_t>(location_px.y()),
                     static_cast<uint32_t>(direction), 0, 0});
}

bool HasWMSpecProperty(const base::flat_set<x11::Atom>& properties,
                       x11::Atom atom) {
  return properties.find(atom) != properties.end();
}

bool GetCustomFramePrefDefault() {

    return false;

}

bool IsWmTiling(WindowManagerName window_manager) {
  switch (window_manager) {
    case WM_BLACKBOX:
    case WM_COMPIZ:
    case WM_ENLIGHTENMENT:
    case WM_FLUXBOX:
    case WM_ICE_WM:
    case WM_KWIN:
    case WM_MATCHBOX:
    case WM_METACITY:
    case WM_MUFFIN:
    case WM_MUTTER:
    case WM_OPENBOX:
    case WM_XFWM4:
      // Stacking window managers.
      return false;

    case WM_I3:
    case WM_ION3:
    case WM_NOTION:
    case WM_RATPOISON:
    case WM_STUMPWM:
      // Tiling window managers.
      return true;

    case WM_AWESOME:
    case WM_QTILE:
    case WM_XMONAD:
    case WM_WMII:
      // Dynamic (tiling and stacking) window managers.  Assume tiling.
      return true;

    case WM_OTHER:
    case WM_UNNAMED:
      // Unknown.  Assume stacking.
      return false;
  }
}

bool GetWindowDesktop(x11::Window window, int32_t* desktop) {
  return GetProperty(window, x11::GetAtom("_NET_WM_DESKTOP"), desktop);
}

WindowManagerName GuessWindowManager() {
  std::string name;
  if (!GetWindowManagerName(&name))
    return WM_UNNAMED;
  // These names are taken from the WMs' source code.
  if (name == "awesome")
    return WM_AWESOME;
  if (name == "Blackbox")
    return WM_BLACKBOX;
  if (name == "Compiz" || name == "compiz")
    return WM_COMPIZ;
  if (name == "e16" || name == "Enlightenment")
    return WM_ENLIGHTENMENT;
  if (name == "Fluxbox")
    return WM_FLUXBOX;
  if (name == "i3")
    return WM_I3;
  if (base::StartsWith(name, "IceWM", base::CompareCase::SENSITIVE))
    return WM_ICE_WM;
  if (name == "ion3")
    return WM_ION3;
  if (name == "KWin")
    return WM_KWIN;
  if (name == "matchbox")
    return WM_MATCHBOX;
  if (name == "Metacity")
    return WM_METACITY;
  if (name == "Mutter (Muffin)")
    return WM_MUFFIN;
  if (name == "GNOME Shell")
    return WM_MUTTER;  // GNOME Shell uses Mutter
  if (name == "Mutter")
    return WM_MUTTER;
  if (name == "notion")
    return WM_NOTION;
  if (name == "Openbox")
    return WM_OPENBOX;
  if (name == "qtile")
    return WM_QTILE;
  if (name == "ratpoison")
    return WM_RATPOISON;
  if (name == "stumpwm")
    return WM_STUMPWM;
  if (name == "wmii")
    return WM_WMII;
  if (name == "Xfwm4")
    return WM_XFWM4;
  if (name == "xmonad")
    return WM_XMONAD;
  return WM_OTHER;
}

std::string GuessWindowManagerName() {
  std::string name;
  if (GetWindowManagerName(&name))
    return name;
  return "Unknown";
}

UMALinuxWindowManager GetWindowManagerUMA() {
  switch (GuessWindowManager()) {
    case WM_OTHER:
      return UMALinuxWindowManager::kOther;
    case WM_UNNAMED:
      return UMALinuxWindowManager::kUnnamed;
    case WM_AWESOME:
      return UMALinuxWindowManager::kAwesome;
    case WM_BLACKBOX:
      return UMALinuxWindowManager::kBlackbox;
    case WM_COMPIZ:
      return UMALinuxWindowManager::kCompiz;
    case WM_ENLIGHTENMENT:
      return UMALinuxWindowManager::kEnlightenment;
    case WM_FLUXBOX:
      return UMALinuxWindowManager::kFluxbox;
    case WM_I3:
      return UMALinuxWindowManager::kI3;
    case WM_ICE_WM:
      return UMALinuxWindowManager::kIceWM;
    case WM_ION3:
      return UMALinuxWindowManager::kIon3;
    case WM_KWIN:
      return UMALinuxWindowManager::kKWin;
    case WM_MATCHBOX:
      return UMALinuxWindowManager::kMatchbox;
    case WM_METACITY:
      return UMALinuxWindowManager::kMetacity;
    case WM_MUFFIN:
      return UMALinuxWindowManager::kMuffin;
    case WM_MUTTER:
      return UMALinuxWindowManager::kMutter;
    case WM_NOTION:
      return UMALinuxWindowManager::kNotion;
    case WM_OPENBOX:
      return UMALinuxWindowManager::kOpenbox;
    case WM_QTILE:
      return UMALinuxWindowManager::kQtile;
    case WM_RATPOISON:
      return UMALinuxWindowManager::kRatpoison;
    case WM_STUMPWM:
      return UMALinuxWindowManager::kStumpWM;
    case WM_WMII:
      return UMALinuxWindowManager::kWmii;
    case WM_XFWM4:
      return UMALinuxWindowManager::kXfwm4;
    case WM_XMONAD:
      return UMALinuxWindowManager::kXmonad;
  }
  NOTREACHED();
  return UMALinuxWindowManager::kOther;
}

bool IsCompositingManagerPresent() {
  auto is_compositing_manager_present_impl = []() {
    auto response = x11::Connection::Get()
                        ->GetSelectionOwner({x11::GetAtom("_NET_WM_CM_S0")})
                        .Sync();
    return response && response->owner != x11::Window::None;
  };

  static bool is_compositing_manager_present =
      is_compositing_manager_present_impl();
  return is_compositing_manager_present;
}

bool IsX11WindowFullScreen(x11::Window window) {
  // If _NET_WM_STATE_FULLSCREEN is in _NET_SUPPORTED, use the presence or
  // absence of _NET_WM_STATE_FULLSCREEN in _NET_WM_STATE to determine
  // whether we're fullscreen.
  x11::Atom fullscreen_atom = x11::GetAtom("_NET_WM_STATE_FULLSCREEN");
  if (WmSupportsHint(fullscreen_atom)) {
    std::vector<x11::Atom> atom_properties;
    if (GetArrayProperty(window, x11::GetAtom("_NET_WM_STATE"),
                         &atom_properties)) {
      return base::Contains(atom_properties, fullscreen_atom);
    }
  }

  gfx::Rect window_rect;
  if (!ui::GetDecoratedWindowBounds(window, &window_rect))
    return false;

  // TODO(thomasanderson): We should use
  // display::Screen::GetDisplayNearestWindow() instead of using the
  // connection screen size, which encompasses all displays.
  auto* connection = x11::Connection::Get();
  int width = connection->default_screen().width_in_pixels;
  int height = connection->default_screen().height_in_pixels;
  return window_rect.size() == gfx::Size(width, height);
}

void SuspendX11ScreenSaver(bool suspend) {
  static const bool kScreenSaverAvailable = IsX11ScreenSaverAvailable();
  if (!kScreenSaverAvailable)
    return;

  x11::Connection::Get()->screensaver().Suspend({suspend});
}

void StoreGpuExtraInfoIntoListValue(x11::VisualId system_visual,
                                    x11::VisualId rgba_visual,
                                    base::Value& list_value) {
  list_value.Append(display::BuildGpuInfoEntry("Window manager",
                                               ui::GuessWindowManagerName()));
  list_value.Append(display::BuildGpuInfoEntry(
      "Compositing manager", ui::IsCompositingManagerPresent() ? "Yes" : "No"));
  list_value.Append(display::BuildGpuInfoEntry(
      "System visual ID",
      base::NumberToString(static_cast<uint32_t>(system_visual))));
  list_value.Append(display::BuildGpuInfoEntry(
      "RGBA visual ID",
      base::NumberToString(static_cast<uint32_t>(rgba_visual))));
}

bool WmSupportsHint(x11::Atom atom) {
  if (!SupportsEWMH())
    return false;

  std::vector<x11::Atom> supported_atoms;
  if (!GetArrayProperty(GetX11RootWindow(), x11::GetAtom("_NET_SUPPORTED"),
                        &supported_atoms)) {
    return false;
  }

  return base::Contains(supported_atoms, atom);
}

gfx::ICCProfile GetICCProfileForMonitor(int monitor) {
  gfx::ICCProfile icc_profile;
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless))
    return icc_profile;
  std::string atom_name = monitor == 0
                              ? "_ICC_PROFILE"
                              : base::StringPrintf("_ICC_PROFILE_%d", monitor);
  scoped_refptr<base::RefCountedMemory> data;
  if (GetRawBytesOfProperty(GetX11RootWindow(), x11::GetAtom(atom_name), &data,
                            nullptr)) {
    icc_profile = gfx::ICCProfile::FromData(data->data(), data->size());
  }
  return icc_profile;
}

bool IsSyncExtensionAvailable() {
// Chrome for ChromeOS can be run with X11 on a Linux desktop. In this case,
// NotifySwapAfterResize is never called as the compositor does not notify about
// swaps after resize. Thus, simply disable usage of XSyncCounter on ChromeOS
// builds.
//
// TODO(https://crbug.com/1036285): Also, disable sync extension for all ozone
// builds as long as our EGL impl for Ozone/X11 is not mature enough and we do
// not receive swap completions on time, which results in weird resize behaviour
// as X Server waits for the XSyncCounter changes.
#if BUILDFLAG(IS_CHROMEOS_ASH) || defined(USE_OZONE)
  return false;
#else
  static bool result =
      x11::Connection::Get()
          ->sync()
          .Initialize({x11::Sync::major_version, x11::Sync::minor_version})
          .Sync();
  return result;
#endif
}

SkColorType ColorTypeForVisual(x11::VisualId visual) {
  struct {
    SkColorType color_type;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    int bpp;
  } color_infos[] = {
      {kRGB_565_SkColorType, 0xf800, 0x7e0, 0x1f, 16},
      {kARGB_4444_SkColorType, 0xf000, 0xf00, 0xf0, 16},
      {kRGBA_8888_SkColorType, 0xff, 0xff00, 0xff0000, 32},
      {kBGRA_8888_SkColorType, 0xff0000, 0xff00, 0xff, 32},
      {kRGBA_1010102_SkColorType, 0x3ff, 0xffc00, 0x3ff00000, 32},
      {kBGRA_1010102_SkColorType, 0x3ff00000, 0xffc00, 0x3ff, 32},
  };
  auto* connection = x11::Connection::Get();
  const auto* vis = connection->GetVisualInfoFromId(visual);
  if (!vis)
    return kUnknown_SkColorType;
  // We don't currently support anything other than TrueColor and DirectColor.
  if (!vis->visual_type->red_mask || !vis->visual_type->green_mask ||
      !vis->visual_type->blue_mask) {
    return kUnknown_SkColorType;
  }
  for (const auto& color_info : color_infos) {
    if (vis->visual_type->red_mask == color_info.red_mask &&
        vis->visual_type->green_mask == color_info.green_mask &&
        vis->visual_type->blue_mask == color_info.blue_mask &&
        vis->format->bits_per_pixel == color_info.bpp) {
      return color_info.color_type;
    }
  }
  LOG(ERROR) << "Unsupported visual with rgb mask 0x" << std::hex
             << vis->visual_type->red_mask << ", 0x"
             << vis->visual_type->green_mask << ", 0x"
             << vis->visual_type->blue_mask
             << ".  Please report this to https://crbug.com/1025266";
  return kUnknown_SkColorType;
}

x11::Future<void> SendClientMessage(x11::Window window,
                                    x11::Window target,
                                    x11::Atom type,
                                    const std::array<uint32_t, 5> data,
                                    x11::EventMask event_mask) {
  x11::ClientMessageEvent event{.format = 32, .window = window, .type = type};
  event.data.data32 = data;
  return SendEvent(event, target, event_mask);
}

bool IsVulkanSurfaceSupported() {
  static const char* extensions[] = {
      "DRI3",         // open source driver.
      "ATIFGLRXDRI",  // AMD proprietary driver.
      "NV-CONTROL",   // NVidia proprietary driver.
  };
  auto* connection = x11::Connection::Get();
  for (const auto* extension : extensions) {
    if (connection->QueryExtension(extension).Sync())
      return true;
  }
  return false;
}

bool DoesVisualHaveAlphaForTest() {
  uint8_t depth = 0;
  bool visual_has_alpha = false;
  ui::XVisualManager::GetInstance()->ChooseVisualForWindow(
      true, nullptr, &depth, nullptr, &visual_has_alpha);

  if (visual_has_alpha)
    DCHECK_EQ(32, depth);

  return visual_has_alpha;
}

gfx::ImageSkia GetNativeWindowIcon(intptr_t target_window_id) {
  std::vector<uint32_t> data;
  if (!GetArrayProperty(static_cast<x11::Window>(target_window_id),
                        x11::GetAtom("_NET_WM_ICON"), &data)) {
    return gfx::ImageSkia();
  }

  // The format of |data| is concatenation of sections like
  // [width, height, pixel data of size width * height], and the total bytes
  // number of |data| is |size|. And here we are picking the largest icon.
  int width = 0;
  int height = 0;
  int start = 0;
  size_t i = 0;
  while (i + 1 < data.size()) {
    if ((static_cast<int>(data[i] * data[i + 1]) > width * height) &&
        (i + 1 + data[i] * data[i + 1] < data.size())) {
      width = static_cast<int>(data[i]);
      height = static_cast<int>(data[i + 1]);
      start = i + 2;
    }
    i += 2 + static_cast<int>(data[i] * data[i + 1]);
  }

  if (width == 0 || height == 0)
    return gfx::ImageSkia();

  SkBitmap result;
  SkImageInfo info = SkImageInfo::MakeN32(width, height, kUnpremul_SkAlphaType);
  result.allocPixels(info);

  uint32_t* pixels_data = reinterpret_cast<uint32_t*>(result.getPixels());

  for (long y = 0; y < height; ++y) {
    for (long x = 0; x < width; ++x) {
      pixels_data[result.rowBytesAsPixels() * y + x] =
          static_cast<uint32_t>(data[start + width * y + x]);
    }
  }

  return gfx::ImageSkia::CreateFrom1xBitmap(result);
}

// static
XVisualManager* XVisualManager::GetInstance() {
  return base::Singleton<XVisualManager>::get();
}

XVisualManager::XVisualManager() {
  auto* connection = x11::Connection::Get();
  for (const auto& depth : connection->default_screen().allowed_depths) {
    for (const auto& visual : depth.visuals) {
      visuals_[visual.visual_id] =
          std::make_unique<XVisualData>(connection, depth.depth, &visual);
    }
  }

  auto* visual_picker = VisualPickerGlx::GetInstance();
  x11::ColorMap colormap;

  // Choose the opaque visual.
  opaque_visual_id_ = visual_picker->system_visual();
  if (opaque_visual_id_ == x11::VisualId{})
    opaque_visual_id_ = connection->default_screen().root_visual;
  // opaque_visual_id_ may be unset in headless environments
  if (opaque_visual_id_ != x11::VisualId{}) {
    DCHECK(visuals_.find(opaque_visual_id_) != visuals_.end());
    ChooseVisualForWindow(false, nullptr, nullptr, &colormap, nullptr);
  }

  // Choose the transparent visual.
  transparent_visual_id_ = visual_picker->rgba_visual();
  if (transparent_visual_id_ == x11::VisualId{}) {
    for (const auto& pair : visuals_) {
      // Why support only 8888 ARGB? Because it's all that GTK+ supports. In
      // gdkvisual-x11.cc, they look for this specific visual and use it for
      // all their alpha channel using needs.
      const auto& data = *pair.second;
      if (data.depth == 32 && data.info->red_mask == 0xff0000 &&
          data.info->green_mask == 0x00ff00 &&
          data.info->blue_mask == 0x0000ff) {
        transparent_visual_id_ = pair.first;
        break;
      }
    }
  }
  if (transparent_visual_id_ != x11::VisualId{}) {
    DCHECK(visuals_.find(transparent_visual_id_) != visuals_.end());
    ChooseVisualForWindow(true, nullptr, nullptr, &colormap, nullptr);
  }
}

XVisualManager::~XVisualManager() = default;

void XVisualManager::ChooseVisualForWindow(bool want_argb_visual,
                                           x11::VisualId* visual_id,
                                           uint8_t* depth,
                                           x11::ColorMap* colormap,
                                           bool* visual_has_alpha) {
  bool use_argb = want_argb_visual && ArgbVisualAvailable();
  x11::VisualId visual = use_argb ? transparent_visual_id_ : opaque_visual_id_;

  if (visual_id)
    *visual_id = visual;
  bool success = GetVisualInfo(visual, depth, colormap, visual_has_alpha);
  DCHECK(success);
}

bool XVisualManager::GetVisualInfo(x11::VisualId visual_id,
                                   uint8_t* depth,
                                   x11::ColorMap* colormap,
                                   bool* visual_has_alpha) {
  DCHECK_NE(visual_id, x11::VisualId{});
  auto it = visuals_.find(visual_id);
  if (it == visuals_.end())
    return false;
  XVisualData& data = *it->second;
  const x11::VisualType& info = *data.info;

  if (depth)
    *depth = data.depth;
  if (colormap) {
    bool is_default_visual =
        visual_id == x11::Connection::Get()->default_root_visual().visual_id;
    *colormap = is_default_visual ? x11::ColorMap{} : data.GetColormap();
  }
  if (visual_has_alpha) {
    auto popcount = [](auto x) {
      return std::bitset<8 * sizeof(decltype(x))>(x).count();
    };
    *visual_has_alpha = popcount(info.red_mask) + popcount(info.green_mask) +
                            popcount(info.blue_mask) <
                        static_cast<std::size_t>(data.depth);
  }
  return true;
}

bool XVisualManager::ArgbVisualAvailable() const {
  return IsCompositingManagerPresent() &&
         transparent_visual_id_ != x11::VisualId{};
}

XVisualManager::XVisualData::XVisualData(x11::Connection* connection,
                                         uint8_t depth,
                                         const x11::VisualType* info)
    : depth(depth), info(info) {}

// Do not free the colormap as this would uninstall the colormap even for
// non-Chromium clients.
XVisualManager::XVisualData::~XVisualData() = default;

x11::ColorMap XVisualManager::XVisualData::GetColormap() {
  if (colormap_ == x11::ColorMap{}) {
    auto* connection = x11::Connection::Get();
    colormap_ = connection->GenerateId<x11::ColorMap>();
    connection->CreateColormap({x11::ColormapAlloc::None, colormap_,
                                connection->default_root(), info->visual_id});
    // In single-process mode, XVisualManager may be used on multiple threads,
    // so we need to flush colormap creation early so that other threads are
    // able to use it.
    connection->Flush();
  }
  return colormap_;
}

ScopedUnsetDisplay::ScopedUnsetDisplay() {
  const char* display = getenv("DISPLAY");
  if (display) {
    display_.emplace(display);
    unsetenv("DISPLAY");
  }
}

ScopedUnsetDisplay::~ScopedUnsetDisplay() {
  if (display_) {
    setenv("DISPLAY", display_->c_str(), 1);
  }
}

}  // namespace ui
