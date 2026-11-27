#include "wayland/toplevel_surface.h"

#include "core/log.h"
#include "wayland/wayland_connection.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <span>
#include <utility>

namespace {

  constexpr Logger kLog("wayland");

  const xdg_surface_listener kXdgSurfaceListener = {
      .configure = &ToplevelSurface::handleXdgSurfaceConfigure,
  };

  const xdg_toplevel_listener kToplevelListener = {
      .configure = &ToplevelSurface::handleToplevelConfigure,
      .close = &ToplevelSurface::handleToplevelClose,
      .configure_bounds = &ToplevelSurface::handleToplevelConfigureBounds,
      .wm_capabilities = &ToplevelSurface::handleToplevelWmCapabilities,
  };

} // namespace

ToplevelSurface::ToplevelSurface(WaylandConnection& connection) : Surface(connection) { setHazeProfile("windows"); }

ToplevelSurface::~ToplevelSurface() {
  m_connection.unregisterSurface(m_surface);
  destroyRoleObjects();
}

bool ToplevelSurface::initialize(wl_output* output, const ToplevelSurfaceConfig& config) {
  if (!m_connection.hasXdgShell()) {
    kLog.warn("toplevel: missing xdg-shell");
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }

  std::int32_t bufferScale = 1;
  if (output != nullptr) {
    if (const auto* wlOutput = m_connection.findOutputByWl(output); wlOutput != nullptr) {
      bufferScale = wlOutput->scale;
    }
    m_connection.registerSurfaceOutput(m_surface, output);
  }
  setBufferScale(bufferScale);

  m_pendingWidth = std::max<std::uint32_t>(1, config.width);
  m_pendingHeight = std::max<std::uint32_t>(1, config.height);
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;

  m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.xdgWmBase(), m_surface);
  if (m_xdgSurface == nullptr) {
    destroySurface();
    return false;
  }
  xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);

  m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
  if (m_toplevel == nullptr) {
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);
  xdg_toplevel_set_title(m_toplevel, config.title.empty() ? "Nythera" : config.title.c_str());
  xdg_toplevel_set_app_id(m_toplevel, config.appId != nullptr ? config.appId : "dev.nythera.Nythera");
  if (config.parent != nullptr)
    xdg_toplevel_set_parent(m_toplevel, config.parent);
  if (config.minWidth > 0 || config.minHeight > 0) {
    xdg_toplevel_set_min_size(
        m_toplevel, static_cast<std::int32_t>(config.minWidth), static_cast<std::int32_t>(config.minHeight)
    );
  }
  if (config.maxWidth > 0 || config.maxHeight > 0) {
    xdg_toplevel_set_max_size(
        m_toplevel, static_cast<std::int32_t>(config.maxWidth), static_cast<std::int32_t>(config.maxHeight)
    );
  }

  wl_surface_commit(m_surface);
  wl_display_flush(m_connection.display());
  if (wl_display_roundtrip(m_connection.display()) < 0) {
    kLog.warn("toplevel: initial roundtrip failed");
    destroyRoleObjects();
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void ToplevelSurface::setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }

void ToplevelSurface::setSuspendedCallback(std::function<void(bool)> callback) {
  m_suspendedCallback = std::move(callback);
}

void ToplevelSurface::setTitle(std::string_view title) {
  if (m_toplevel == nullptr) {
    return;
  }
  const std::string resolved = title.empty() ? "Nythera" : std::string(title);
  xdg_toplevel_set_title(m_toplevel, resolved.c_str());
}

void ToplevelSurface::setMinSize(std::uint32_t minWidth, std::uint32_t minHeight) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_set_min_size(m_toplevel, static_cast<std::int32_t>(minWidth), static_cast<std::int32_t>(minHeight));
  }
}

void ToplevelSurface::resizeClient(std::uint32_t width, std::uint32_t height) {
  if (!isRunning() || width == 0 || height == 0 || (this->width() == width && this->height() == height)) {
    return;
  }
  m_pendingWidth = width;
  m_pendingHeight = height;
  m_lastToplevelWidth = 0;
  m_lastToplevelHeight = 0;
  Surface::onConfigure(width, height);
}

void ToplevelSurface::ModalHintDeleter::operator()(xdg_dialog_v1* dialog) const { xdg_dialog_v1_destroy(dialog); }

void ToplevelSurface::ModalHintDeleter::operator()(xdg_wm_dialog_v1* manager) const {
  xdg_wm_dialog_v1_destroy(manager);
}

ToplevelSurface::ModalManager ToplevelSurface::bindModalManager(WaylandConnection& connection) {
  // Discover the optional extension without dispatching unrelated application events.
  auto* display = connection.display();
  auto* queue = wl_display_create_queue(display);
  auto* wrapper = wl_proxy_create_wrapper(display);
  if (queue == nullptr || wrapper == nullptr) {
    if (wrapper != nullptr)
      wl_proxy_wrapper_destroy(wrapper);
    if (queue != nullptr)
      wl_event_queue_destroy(queue);
    return {};
  }
  wl_proxy_set_queue(static_cast<wl_proxy*>(wrapper), queue);
  auto* registry = wl_display_get_registry(static_cast<wl_display*>(wrapper));
  wl_proxy_wrapper_destroy(wrapper);
  ModalManager manager;
  constexpr wl_registry_listener listener{
      .global =
          [](void* data, wl_registry* source, std::uint32_t name, const char* interface, std::uint32_t) {
            auto& bound = *static_cast<ModalManager*>(data);
            if (!bound && std::string_view(interface) == xdg_wm_dialog_v1_interface.name)
              bound.reset(
                  static_cast<xdg_wm_dialog_v1*>(wl_registry_bind(source, name, &xdg_wm_dialog_v1_interface, 1))
              );
          },
      .global_remove = [](void*, wl_registry*, std::uint32_t) {},
  };
  if (registry != nullptr) {
    wl_registry_add_listener(registry, &listener, &manager);
    if (wl_display_roundtrip_queue(display, queue) < 0)
      manager.reset();
    wl_registry_destroy(registry);
  }
  if (manager)
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(manager.get()), nullptr);
  wl_event_queue_destroy(queue);
  return manager;
}

ToplevelSurface::ModalHint ToplevelSurface::createModalHint(xdg_wm_dialog_v1* manager) const {
  if (manager == nullptr || m_toplevel == nullptr)
    return {};
  ModalHint hint(xdg_wm_dialog_v1_get_xdg_dialog(manager, m_toplevel));
  if (hint)
    xdg_dialog_v1_set_modal(hint.get());
  return hint;
}

void ToplevelSurface::beginMove(std::uint32_t serial) {
  if (m_toplevel != nullptr) {
    xdg_toplevel_move(m_toplevel, m_connection.seat(), serial);
  }
}

void ToplevelSurface::beginResize(std::uint32_t serial) {
  if (m_toplevel != nullptr)
    xdg_toplevel_resize(m_toplevel, m_connection.seat(), serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
}

void ToplevelSurface::handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto* self = static_cast<ToplevelSurface*>(data);
  xdg_surface_ack_configure(surface, serial);

  std::uint32_t width = self->m_pendingWidth;
  std::uint32_t height = self->m_pendingHeight;
  if (self->m_lastToplevelWidth > 0) {
    width = static_cast<std::uint32_t>(self->m_lastToplevelWidth);
  }
  if (self->m_lastToplevelHeight > 0) {
    height = static_cast<std::uint32_t>(self->m_lastToplevelHeight);
  }
  width = std::max<std::uint32_t>(1, width);
  height = std::max<std::uint32_t>(1, height);
  self->Surface::onConfigure(width, height);
}

void ToplevelSurface::handleToplevelConfigure(
    void* data, xdg_toplevel* /*toplevel*/, std::int32_t width, std::int32_t height, wl_array* states
) {
  auto* self = static_cast<ToplevelSurface*>(data);
  bool suspended = false;
  if (states) {
    const std::span values{static_cast<const std::uint32_t*>(states->data), states->size / sizeof(std::uint32_t)};
    suspended = std::ranges::find(values, XDG_TOPLEVEL_STATE_SUSPENDED) != values.end();
  }
  if (self->m_suspended != suspended) {
    self->m_suspended = suspended;
    self->setHazeSuspended(suspended);
    if (self->m_suspendedCallback)
      self->m_suspendedCallback(suspended);
  }
  self->m_lastToplevelWidth = width;
  self->m_lastToplevelHeight = height;
  if (width > 0) {
    self->m_pendingWidth = static_cast<std::uint32_t>(width);
  }
  if (height > 0) {
    self->m_pendingHeight = static_cast<std::uint32_t>(height);
  }
}

void ToplevelSurface::handleToplevelClose(void* data, xdg_toplevel* /*toplevel*/) {
  auto* self = static_cast<ToplevelSurface*>(data);
  self->setRunning(false);
  if (self->m_closedCallback) {
    self->m_closedCallback();
  }
}

void ToplevelSurface::handleToplevelConfigureBounds(
    void* /*data*/, xdg_toplevel* /*toplevel*/, std::int32_t /*width*/, std::int32_t /*height*/
) {}

void ToplevelSurface::
    handleToplevelWmCapabilities(void* /*data*/, xdg_toplevel* /*toplevel*/, wl_array* /*capabilities*/) {}

void ToplevelSurface::destroyRoleObjects() {
  if (m_toplevel != nullptr) {
    xdg_toplevel_destroy(m_toplevel);
    m_toplevel = nullptr;
  }
  if (m_xdgSurface != nullptr) {
    xdg_surface_destroy(m_xdgSurface);
    m_xdgSurface = nullptr;
  }
}
