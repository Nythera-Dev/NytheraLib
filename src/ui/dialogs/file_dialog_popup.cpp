#include "ui/dialogs/file_dialog_popup.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>

namespace {

  constexpr float kToplevelResizeEdgeThickness = 8.0F;
  constexpr float kToplevelMinWidth = 520.0F;
  constexpr float kToplevelMinHeight = 360.0F;

} // namespace

FileDialogPopup::FileDialogPopup() = default;

FileDialogPopup::~FileDialogPopup() {
  destroyToplevel();
  destroyPopup();
}

void FileDialogPopup::initialize(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts,
    ThumbnailService& thumbnails
) {
  initializeBase(wayland, config, renderContext, popupHosts);
  setHazeProfile("file-dialog");
  m_thumbnails = &thumbnails;
}

bool FileDialogPopup::shouldUseToplevelSurface(const std::optional<LayerPopupParentContext>& parent) noexcept {
  return parent.has_value() && parent->xdgToplevel != nullptr;
}

bool FileDialogPopup::isOpen() const noexcept { return m_toplevelSurface != nullptr || DialogPopupHost::isOpen(); }

bool FileDialogPopup::ownsSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return false;
  }
  if (m_toplevelSurface != nullptr) {
    return surface == m_toplevelSurface->wlSurface();
  }
  return surface == DialogPopupHost::wlSurface();
}

bool FileDialogPopup::handlesKeyboardForSurface(wl_surface* keyboardSurface) const noexcept {
  if (!isOpen()) {
    return false;
  }
  if (m_toplevelSurface != nullptr) {
    return ownsSurface(keyboardSurface);
  }
  return true;
}

wl_surface* FileDialogPopup::wlSurface() const noexcept {
  return m_toplevelSurface != nullptr ? m_toplevelSurface->wlSurface() : DialogPopupHost::wlSurface();
}

bool FileDialogPopup::openFileDialog() {
  if (m_thumbnails == nullptr) {
    return false;
  }

  destroyToplevel();
  destroyPopup();

  // FileDialogView is constructed before opening a surface so we can query its
  // preferred size and so scene population has a live m_dialog to attach.
  auto dialog = std::make_unique<FileDialogView>(m_thumbnails);
  dialog->setHost(this);
  dialog->setContentScale(uiScale());

  const auto width = static_cast<std::uint32_t>(dialog->preferredWidth());
  const auto height = static_cast<std::uint32_t>(dialog->preferredHeight());
  const auto parentContext = (m_popupHosts != nullptr && m_wayland != nullptr)
      ? m_popupHosts->resolveForInput(*m_wayland)
      : std::optional<LayerPopupParentContext>{};

  m_dialog = std::move(dialog);
  if (shouldUseToplevelSurface(parentContext)) {
    if (!openToplevel(*parentContext, width, height)) {
      m_dialog.reset();
      return false;
    }
    return true;
  }

  if (!openPopup(width, height)) {
    m_dialog.reset();
    return false;
  }
  return true;
}

void FileDialogPopup::closeFileDialogWithoutResult() {
  destroyToplevel();
  destroyPopup();
}

bool FileDialogPopup::openToplevel(const LayerPopupParentContext& parent, std::uint32_t width, std::uint32_t height) {
  if (m_wayland == nullptr || m_renderContext == nullptr || parent.xdgToplevel == nullptr) {
    return false;
  }

  const float scale = m_dialog != nullptr ? m_dialog->contentScale() : uiScale();
  const auto minWidth = static_cast<std::uint32_t>(std::round(kToplevelMinWidth * scale));
  const auto minHeight = static_cast<std::uint32_t>(std::round(kToplevelMinHeight * scale));
  const auto clampedWidth = std::max(width, minWidth);
  const auto clampedHeight = std::max(height, minHeight);

  auto surface = std::make_unique<ToplevelSurface>(*m_wayland);
  surface->setHazeProfile(m_hazeProfile);
  surface->setRenderContext(m_renderContext);
  surface->setAnimationManager(&animations());
  surface->setConfigureCallback([this](std::uint32_t /*w*/, std::uint32_t /*h*/) { requestLayout(); });
  surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareToplevelFrame(needsUpdate, needsLayout);
  });

  m_toplevelAliveGuard = std::make_shared<int>(0);
  const std::weak_ptr<void> aliveGuard = m_toplevelAliveGuard;
  surface->setClosedCallback([this, aliveGuard] {
    DeferredCall::callLater([this, aliveGuard] {
      if (aliveGuard.expired()) {
        return;
      }
      cancel();
    });
  });

  const std::string& requestedTitle = FileDialog::currentOptions().title;
  ToplevelSurfaceConfig config{
      .width = clampedWidth,
      .height = clampedHeight,
      .minWidth = minWidth,
      .minHeight = minHeight,
      .parent = parent.xdgToplevel,
      .title = requestedTitle.empty() ? std::string("File Dialog") : requestedTitle,
      .appId = "dev.nythera.FileDialog",
  };
  if (!surface->initialize(parent.output, config)) {
    m_toplevelAliveGuard.reset();
    return false;
  }

  m_toplevelSurface = std::move(surface);
  m_toplevelSurface->requestLayout();
  return true;
}

void FileDialogPopup::destroyToplevel() {
  if (m_toplevelSurface == nullptr && m_toplevelSceneRoot == nullptr) {
    m_toplevelAliveGuard.reset();
    return;
  }

  m_toplevelAliveGuard.reset();
  m_pointerInside = false;
  inputDispatcher().setTextInputContext(nullptr, nullptr);
  inputDispatcher().setSceneRoot(nullptr);
  if (m_toplevelSurface != nullptr) {
    m_toplevelSurface->setSceneRoot(nullptr);
  }
  onSheetClose();
  m_toplevelBg = nullptr;
  m_toplevelContent = nullptr;
  m_toplevelResizeEdges.fill(nullptr);
  m_toplevelSceneRoot.reset();
  m_toplevelSurface.reset();
}

void FileDialogPopup::prepareToplevelFrame(bool needsUpdate, bool needsLayout) {
  if (m_toplevelSurface == nullptr || m_renderContext == nullptr) {
    return;
  }

  const auto width = m_toplevelSurface->width();
  const auto height = m_toplevelSurface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(m_toplevelSurface->renderTarget());

  if (m_toplevelSceneRoot == nullptr) {
    buildToplevelScene(width, height);
  }
  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    runUpdatePhase();
  }
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutToplevelScene(static_cast<float>(width), static_cast<float>(height));
  }
  if (needsUpdate && m_toplevelSceneRoot != nullptr && m_toplevelSceneRoot->layoutDirty()) {
    requestLayout();
  }
}

void FileDialogPopup::buildToplevelScene(std::uint32_t width, std::uint32_t height) {
  m_toplevelSceneRoot = ui::node({});
  m_toplevelSceneRoot->setAnimationManager(&animations());

  auto bg = ui::box({
      .configure = [](Box& box) { box.setDialogStyle(); },
  });
  m_toplevelBg = static_cast<Box*>(m_toplevelSceneRoot->addChild(std::move(bg)));

  auto content = ui::node({});
  m_toplevelContent = m_toplevelSceneRoot->addChild(std::move(content));

  static constexpr std::array<std::uint32_t, 8> kResizeEdges{
      XDG_TOPLEVEL_RESIZE_EDGE_TOP,         XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,       XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
      XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,       XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,     XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
      XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
  };
  static constexpr std::array<std::uint32_t, 8> kResizeCursors{
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE,
  };
  for (std::size_t i = 0; i < kResizeEdges.size(); ++i) {
    auto edge = std::make_unique<InputArea>();
    edge->setParticipatesInLayout(false);
    edge->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    edge->setCursorShape(kResizeCursors[i]);
    edge->setOnPress([this, side = kResizeEdges[i]](const InputArea::PointerData& data) {
      if (data.pressed && m_toplevelSurface != nullptr && m_wayland != nullptr) {
        xdg_toplevel_resize(m_toplevelSurface->xdgToplevel(), m_wayland->seat(), data.serial, side);
      }
    });
    m_toplevelResizeEdges[i] = static_cast<InputArea*>(m_toplevelSceneRoot->addChild(std::move(edge)));
  }

  inputDispatcher().setSceneRoot(m_toplevelSceneRoot.get());
  inputDispatcher().setTextInputContext(m_toplevelSurface->wlSurface(), m_wayland->textInputService());
  inputDispatcher().setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    if (m_wayland != nullptr) {
      m_wayland->setCursorShape(serial, shape);
    }
  });
  m_toplevelSurface->setSceneRoot(m_toplevelSceneRoot.get());

  const float padding = computePadding(uiScale());
  const float contentW = std::max(1.0F, static_cast<float>(width) - padding * 2.0F);
  const float contentH = std::max(1.0F, static_cast<float>(height) - padding * 2.0F);
  populateContent(
      m_toplevelContent, static_cast<std::uint32_t>(std::round(contentW)),
      static_cast<std::uint32_t>(std::round(contentH))
  );

  if (auto* focusArea = initialFocusArea(); focusArea != nullptr) {
    inputDispatcher().setFocus(focusArea);
  }

  layoutToplevelScene(static_cast<float>(width), static_cast<float>(height));

  const std::weak_ptr<void> aliveGuard = m_toplevelAliveGuard;
  DeferredCall::callLater([this, aliveGuard] {
    if (aliveGuard.expired() || m_toplevelSurface == nullptr) {
      return;
    }
    if (auto* focusArea = initialFocusArea(); focusArea != nullptr) {
      inputDispatcher().setFocus(focusArea);
    }
  });
}

void FileDialogPopup::layoutToplevelScene(float width, float height) {
  if (m_toplevelSceneRoot == nullptr || m_toplevelSurface == nullptr) {
    return;
  }

  m_toplevelSceneRoot->setSize(width, height);
  if (m_toplevelBg != nullptr) {
    m_toplevelBg->setPosition(0.0F, 0.0F);
    m_toplevelBg->setSize(width, height);
  }

  const float padding = computePadding(uiScale());
  const float contentW = std::max(1.0F, width - padding * 2.0F);
  const float contentH = std::max(1.0F, height - padding * 2.0F);
  if (m_toplevelContent != nullptr) {
    m_toplevelContent->setPosition(padding, padding);
    m_toplevelContent->setSize(contentW, contentH);
  }

  layoutSheet(contentW, contentH);

  Renderer& renderer = activeRenderer();
  const float edge = kToplevelResizeEdgeThickness;
  const std::array<std::array<float, 4>, 8> bounds{{
      {edge, 0.0F, std::max(0.0F, width - edge * 2.0F), edge},
      {edge, std::max(0.0F, height - edge), std::max(0.0F, width - edge * 2.0F), edge},
      {0.0F, edge, edge, std::max(0.0F, height - edge * 2.0F)},
      {std::max(0.0F, width - edge), edge, edge, std::max(0.0F, height - edge * 2.0F)},
      {0.0F, 0.0F, edge, edge},
      {std::max(0.0F, width - edge), 0.0F, edge, edge},
      {0.0F, std::max(0.0F, height - edge), edge, edge},
      {std::max(0.0F, width - edge), std::max(0.0F, height - edge), edge, edge},
  }};
  for (std::size_t i = 0; i < bounds.size(); ++i) {
    if (m_toplevelResizeEdges[i] == nullptr) {
      continue;
    }
    m_toplevelResizeEdges[i]->setPosition(bounds[i][0], bounds[i][1]);
    m_toplevelResizeEdges[i]->setSize(bounds[i][2], bounds[i][3]);
    m_toplevelResizeEdges[i]->layout(renderer);
  }
}

void FileDialogPopup::markToplevelDirtyTail() {
  if (m_toplevelSceneRoot == nullptr || m_toplevelSurface == nullptr) {
    return;
  }
  if (!m_toplevelSceneRoot->paintDirty() && !m_toplevelSceneRoot->layoutDirty()) {
    return;
  }
  if (m_toplevelSceneRoot->layoutDirty()) {
    requestLayout();
  } else {
    requestRedraw();
  }
}

Renderer& FileDialogPopup::activeRenderer() const noexcept {
  return m_toplevelSurface != nullptr ? m_toplevelSurface->renderTarget().renderer() : DialogPopupHost::renderer();
}

bool FileDialogPopup::onPointerEvent(const PointerEvent& event) {
  if (m_toplevelSurface == nullptr) {
    return DialogPopupHost::onPointerEvent(event);
  }

  wl_surface* const ownSurface = m_toplevelSurface->wlSurface();
  if (event.surface != ownSurface) {
    return false;
  }

  const float x = static_cast<float>(event.sx);
  const float y = static_cast<float>(event.sy);
  const bool captured = inputDispatcher().pointerCaptured();

  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    inputDispatcher().pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    if (!captured) {
      inputDispatcher().pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (!captured && !m_pointerInside) {
      m_pointerInside = true;
      inputDispatcher().pointerEnter(x, y, event.serial);
    } else {
      inputDispatcher().pointerMotion(x, y, event.serial);
    }
    break;
  case PointerEvent::Type::Button: {
    if (!captured && !m_pointerInside) {
      m_pointerInside = true;
      inputDispatcher().pointerEnter(x, y, event.serial);
    } else {
      inputDispatcher().pointerMotion(x, y, event.serial);
    }
    const float scale = m_dialog != nullptr ? m_dialog->contentScale() : uiScale();
    const float headerBottom = computePadding(scale) + (Style::spaceMd + Style::controlHeightSm) * scale;
    const float surfW = static_cast<float>(m_toplevelSurface->width());
    if (event.pressed
        && !captured
        && event.button == BTN_LEFT
        && y >= kToplevelResizeEdgeThickness
        && y < headerBottom
        && x >= kToplevelResizeEdgeThickness
        && x < surfW - kToplevelResizeEdgeThickness
        && inputDispatcher().inputAreaAt(x, y) == nullptr) {
      m_toplevelSurface->beginMove(event.serial);
      markToplevelDirtyTail();
      return true;
    }
    inputDispatcher().pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  }
  case PointerEvent::Type::Axis:
    if (!captured && !m_pointerInside) {
      m_pointerInside = true;
      inputDispatcher().pointerEnter(x, y, event.serial);
    } else {
      inputDispatcher().pointerMotion(x, y, event.serial);
    }
    inputDispatcher().pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  markToplevelDirtyTail();
  return true;
}

void FileDialogPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_toplevelSurface == nullptr) {
    DialogPopupHost::onKeyboardEvent(event);
    return;
  }

  if (!event.repeat
      && event.pressed
      && !event.preedit
      && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    cancel();
    return;
  }

  if (preDispatchKeyboard(event)) {
    markToplevelDirtyTail();
    return;
  }

  inputDispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
  markToplevelDirtyTail();
}

void FileDialogPopup::requestUpdateOnly() {
  if (m_toplevelSurface != nullptr) {
    m_toplevelSurface->requestUpdateOnly();
  } else {
    DialogPopupHost::requestUpdateOnly();
  }
}

void FileDialogPopup::requestLayout() {
  if (m_toplevelSurface != nullptr) {
    m_toplevelSurface->requestLayout();
  } else {
    DialogPopupHost::requestLayout();
  }
}

void FileDialogPopup::requestRedraw() {
  if (m_toplevelSurface != nullptr) {
    m_toplevelSurface->requestRedraw();
  } else {
    DialogPopupHost::requestRedraw();
  }
}

void FileDialogPopup::populateContent(Node* contentParent, std::uint32_t /*width*/, std::uint32_t /*height*/) {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->setAnimationManager(&animations());
  m_dialog->create();
  m_dialog->onOpen({});
  if (m_dialog->root() != nullptr) {
    contentParent->addChild(m_dialog->releaseRoot());
  }
}

void FileDialogPopup::layoutSheet(float contentWidth, float contentHeight) {
  if (m_dialog == nullptr) {
    return;
  }
  m_dialog->layout(activeRenderer(), contentWidth, contentHeight);
}

void FileDialogPopup::cancelToFacade() { FileDialog::cancelIfPending(); }

InputArea* FileDialogPopup::initialFocusArea() { return m_dialog != nullptr ? m_dialog->initialFocusArea() : nullptr; }

float FileDialogPopup::computePadding(float /*scale*/) const {
  // Track the dialog's own contentScale to match prior behavior; this
  // happens to equal uiScale() in practice (set in openFileDialog) but
  // staying consistent with FileDialogView's view of the world.
  return m_dialog != nullptr ? m_dialog->contentScale() * 12.0F : 0.0F;
}

void FileDialogPopup::runUpdatePhase() {
  if (m_dialog != nullptr) {
    m_dialog->update(activeRenderer());
  }
}

bool FileDialogPopup::preDispatchKeyboard(const KeyboardEvent& event) {
  if (m_dialog == nullptr) {
    return false;
  }
  return m_dialog->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit, event.repeat);
}

void FileDialogPopup::onSheetClose() {
  if (m_dialog != nullptr) {
    m_dialog->onClose();
    m_dialog.reset();
  }
}

void FileDialogPopup::focusArea(InputArea* area) { inputDispatcher().setFocus(area); }

InputArea* FileDialogPopup::focusedArea() const { return inputDispatcher().focusedArea(); }

void FileDialogPopup::accept(std::optional<std::filesystem::path> result) {
  if (m_toplevelSurface != nullptr) {
    destroyToplevel();
  } else {
    closeAfterAccept();
  }
  FileDialog::complete(std::move(result));
}

void FileDialogPopup::cancel() {
  if (m_toplevelSurface != nullptr) {
    destroyToplevel();
    cancelToFacade();
    return;
  }
  DialogPopupHost::cancel();
}
