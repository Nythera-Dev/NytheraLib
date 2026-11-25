#pragma once

#include "ui/dialogs/dialog_popup_host.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/file_dialog_view.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

class Box;
class InputArea;
class Node;
class ThumbnailService;
class ToplevelSurface;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;

class FileDialogPopup final : public DialogPopupHost, public FileDialogHost, public FileDialogPresenter {
public:
  FileDialogPopup();
  ~FileDialogPopup() override;

  void initialize(
      WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts, ThumbnailService& thumbnails
  );

  [[nodiscard]] static bool shouldUseToplevelSurface(const std::optional<LayerPopupParentContext>& parent) noexcept;

  [[nodiscard]] bool isOpen() const noexcept;
  [[nodiscard]] bool usesToplevelSurface() const noexcept { return m_toplevelSurface != nullptr; }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool handlesKeyboardForSurface(wl_surface* keyboardSurface) const noexcept;
  [[nodiscard]] wl_surface* wlSurface() const noexcept;

  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] bool openFileDialog() override;
  void closeFileDialogWithoutResult() override;

  // FileDialogHost overrides. Route invalidation/completion to ToplevelSurface when
  // opened from an application window, otherwise delegate to DialogPopupHost.
  void requestUpdateOnly() override;
  void requestLayout() override;
  void requestRedraw() override;
  void focusArea(InputArea* area) override;
  [[nodiscard]] InputArea* focusedArea() const override;
  void accept(std::optional<std::filesystem::path> result) override;
  void cancel() override;

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;

  [[nodiscard]] float computePadding(float scale) const override;
  void runUpdatePhase() override;
  [[nodiscard]] bool preDispatchKeyboard(const KeyboardEvent& event) override;
  void onSheetClose() override;

private:
  [[nodiscard]] bool openToplevel(const LayerPopupParentContext& parent, std::uint32_t width, std::uint32_t height);
  void destroyToplevel();
  void prepareToplevelFrame(bool needsUpdate, bool needsLayout);
  void buildToplevelScene(std::uint32_t width, std::uint32_t height);
  void layoutToplevelScene(float width, float height);
  void markToplevelDirtyTail();
  [[nodiscard]] Renderer& activeRenderer() const noexcept;

  ThumbnailService* m_thumbnails = nullptr;
  std::unique_ptr<FileDialogView> m_dialog;
  std::unique_ptr<ToplevelSurface> m_toplevelSurface;
  std::unique_ptr<Node> m_toplevelSceneRoot;
  Box* m_toplevelBg = nullptr;
  Node* m_toplevelContent = nullptr;
  std::array<InputArea*, 8> m_toplevelResizeEdges{};
  std::shared_ptr<void> m_toplevelAliveGuard;
};
