#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "test_check.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/file_dialog_popup.h"
#include "ui/dialogs/file_dialog_view.h"
#include "ui/dialogs/layer_popup_host.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

  class StubPresenter final : public FileDialogPresenter {
  public:
    bool openResult = true;
    int openCalls = 0;
    int closeCalls = 0;

    [[nodiscard]] bool openFileDialog() override {
      ++openCalls;
      return openResult;
    }

    void closeFileDialogWithoutResult() override { ++closeCalls; }
  };

  class StubViewHost final : public FileDialogHost {
  public:
    std::optional<std::filesystem::path> acceptedResult;
    int acceptCalls = 0;
    int cancelCalls = 0;
    InputArea* focused = nullptr;

    void reset() {
      acceptedResult.reset();
      acceptCalls = 0;
      cancelCalls = 0;
      focused = nullptr;
    }

    void requestUpdateOnly() override {}
    void requestLayout() override {}
    void requestRedraw() override {}
    void focusArea(InputArea* area) override { focused = area; }
    [[nodiscard]] InputArea* focusedArea() const override { return focused; }
    void accept(std::optional<std::filesystem::path> result) override {
      ++acceptCalls;
      acceptedResult = std::move(result);
    }
    void cancel() override { ++cancelCalls; }
  };

  void writeEmptyFile(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    TEST_CHECK(out.is_open());
  }

} // namespace

class FileDialogViewTestAccess {
public:
  static std::optional<std::size_t> indexOf(const FileDialogView& view, std::string_view name) {
    for (std::size_t i = 0; i < view.m_visibleEntries.size(); ++i) {
      if (view.m_visibleEntries[i].name == name) {
        return i;
      }
    }
    return std::nullopt;
  }

  static void clickEntry(FileDialogView& view, std::size_t index) { view.handleEntryClick(index); }
  static void activate(FileDialogView& view) { view.activateSelection(); }
  static void submit(FileDialogView& view) { view.submitDialog(); }
  static void clearSelection(FileDialogView& view) {
    view.m_selectedIndex = static_cast<std::size_t>(-1);
    view.updateControls();
  }
  static bool isSelectable(const FileDialogView& view, std::size_t index) { return view.isSelectableIndex(index); }
  static std::size_t selectedIndex(const FileDialogView& view) { return view.m_selectedIndex; }
  static const std::filesystem::path& currentDirectory(const FileDialogView& view) { return view.m_currentDirectory; }
  static Button* okButton(const FileDialogView& view) { return view.m_okButton; }
  static Input* filenameInput(const FileDialogView& view) { return view.m_filenameInput; }
  static Label* titleLabel(const FileDialogView& view) { return view.m_titleLabel; }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-file-dialog-mode"});
    const auto& work = env.work();
    const auto subDir = work / "sub_dir";
    const auto nestedDir = subDir / "nested_dir";
    const auto emptyDir = work / "empty_dir";
    const auto targetFile = work / "target.txt";
    const auto nestedFile = subDir / "nested.txt";

    TEST_CHECK(std::filesystem::create_directory(subDir));
    TEST_CHECK(std::filesystem::create_directory(nestedDir));
    TEST_CHECK(std::filesystem::create_directory(emptyDir));
    writeEmptyFile(targetFile);
    writeEmptyFile(nestedFile);

    i18n::Service::instance().init("en");

    StubPresenter presenter;
    FileDialog::setPresenter(&presenter);
    const auto cleanup = nythera::test::makeScopeExit([] {
      FileDialog::cancelIfPending();
      FileDialog::setPresenter(nullptr);
    });

    StubViewHost host;
    FileDialogView view(nullptr);
    view.setHost(&host);

    const auto openDialogWith = [&](FileDialogOptions options, std::optional<std::filesystem::path>& callbackOut) {
      callbackOut.reset();
      host.reset();
      TEST_CHECK(FileDialog::open(std::move(options), [&](std::optional<std::filesystem::path> res) {
        callbackOut = std::move(res);
      }));
      view.create();
      view.onOpen({});
    };

    // 1. SelectFileOrFolder defaults, controls, folder selection + navigation, file selection, and empty folder
    // confirm.
    std::optional<std::filesystem::path> callbackResult;
    openDialogWith({.mode = FileDialogMode::SelectFileOrFolder, .startDirectory = work}, callbackResult);

    TEST_CHECK(FileDialog::currentOptions().title == "Select File or Folder");
    TEST_CHECK(FileDialogViewTestAccess::titleLabel(view)->text() == "Select File or Folder");
    TEST_CHECK(!FileDialogViewTestAccess::filenameInput(view)->visible());
    TEST_CHECK(FileDialogViewTestAccess::okButton(view)->enabled());
    TEST_CHECK(FileDialogViewTestAccess::okButton(view)->label()->text() == "Select");

    const auto emptyDirIdx = FileDialogViewTestAccess::indexOf(view, "empty_dir");
    const auto subDirIdx = FileDialogViewTestAccess::indexOf(view, "sub_dir");
    const auto fileIdx = FileDialogViewTestAccess::indexOf(view, "target.txt");
    TEST_CHECK(emptyDirIdx.has_value() && subDirIdx.has_value() && fileIdx.has_value());
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *emptyDirIdx));
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *subDirIdx));
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *fileIdx));

    // Single-click file selects it; confirm picks the file.
    FileDialogViewTestAccess::clickEntry(view, *fileIdx);
    TEST_CHECK(FileDialogViewTestAccess::selectedIndex(view) == *fileIdx);
    TEST_CHECK(host.acceptCalls == 0);
    FileDialogViewTestAccess::submit(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == targetFile);

    // Clicking an already-selected file submits it immediately.
    host.reset();
    FileDialogViewTestAccess::clickEntry(view, *fileIdx);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == targetFile);

    // Single-click folder selects it without navigating; confirm picks the folder.
    host.reset();
    FileDialogViewTestAccess::clickEntry(view, *subDirIdx);
    TEST_CHECK(FileDialogViewTestAccess::currentDirectory(view) == work);
    TEST_CHECK(FileDialogViewTestAccess::selectedIndex(view) == *subDirIdx);
    TEST_CHECK(host.acceptCalls == 0);
    FileDialogViewTestAccess::submit(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == subDir);

    // Pressing Enter/Validate on selected folder in SelectFileOrFolder confirms that folder.
    host.reset();
    FileDialogViewTestAccess::activate(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == subDir);

    // Clicking an already-selected folder navigates into it.
    host.reset();
    FileDialogViewTestAccess::clickEntry(view, *subDirIdx);
    TEST_CHECK(host.acceptCalls == 0);
    TEST_CHECK(FileDialogViewTestAccess::currentDirectory(view) == subDir);
    const auto nestedDirIdx = FileDialogViewTestAccess::indexOf(view, "nested_dir");
    const auto nestedFileIdx = FileDialogViewTestAccess::indexOf(view, "nested.txt");
    TEST_CHECK(nestedDirIdx.has_value() && nestedFileIdx.has_value());

    // Confirming with no selection submits current directory.
    FileDialogViewTestAccess::clearSelection(view);
    TEST_CHECK(FileDialogViewTestAccess::okButton(view)->enabled());
    FileDialogViewTestAccess::submit(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == subDir);

    // Pressing Enter/Validate in an empty directory submits that directory.
    view.onClose();
    openDialogWith({.mode = FileDialogMode::SelectFileOrFolder, .startDirectory = emptyDir}, callbackResult);
    FileDialogViewTestAccess::activate(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == emptyDir);
    view.onClose();
    FileDialog::cancelIfPending();

    // 2. Regression check: Open mode keeps files-only selection, visible filename bar, and immediate folder navigation.
    openDialogWith({.mode = FileDialogMode::Open, .startDirectory = work}, callbackResult);
    TEST_CHECK(FileDialog::currentOptions().title == "Open File");
    TEST_CHECK(FileDialogViewTestAccess::filenameInput(view)->visible());
    const auto openSubDirIdx = FileDialogViewTestAccess::indexOf(view, "sub_dir");
    const auto openFileIdx = FileDialogViewTestAccess::indexOf(view, "target.txt");
    TEST_CHECK(openSubDirIdx.has_value() && openFileIdx.has_value());
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *openSubDirIdx));
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *openFileIdx));
    FileDialogViewTestAccess::clickEntry(view, *openSubDirIdx);
    TEST_CHECK(FileDialogViewTestAccess::currentDirectory(view) == subDir);
    view.onClose();
    FileDialog::cancelIfPending();

    // 3. Regression check: SelectFolder remains directory-only with Select Folder button and hidden filename bar.
    openDialogWith({.mode = FileDialogMode::SelectFolder, .startDirectory = work}, callbackResult);
    TEST_CHECK(FileDialog::currentOptions().title == "Select Folder");
    TEST_CHECK(!FileDialogViewTestAccess::filenameInput(view)->visible());
    TEST_CHECK(FileDialogViewTestAccess::okButton(view)->label()->text() == "Select Folder");
    const auto folderOnlyDirIdx = FileDialogViewTestAccess::indexOf(view, "sub_dir");
    const auto folderOnlyFileIdx = FileDialogViewTestAccess::indexOf(view, "target.txt");
    TEST_CHECK(folderOnlyDirIdx.has_value() && folderOnlyFileIdx.has_value());
    TEST_CHECK(FileDialogViewTestAccess::isSelectable(view, *folderOnlyDirIdx));
    TEST_CHECK(!FileDialogViewTestAccess::isSelectable(view, *folderOnlyFileIdx));
    FileDialogViewTestAccess::clickEntry(view, *folderOnlyFileIdx);
    TEST_CHECK(host.acceptCalls == 0);
    FileDialogViewTestAccess::clickEntry(view, *folderOnlyDirIdx);
    TEST_CHECK(FileDialogViewTestAccess::currentDirectory(view) == work);
    TEST_CHECK(FileDialogViewTestAccess::selectedIndex(view) == *folderOnlyDirIdx);
    FileDialogViewTestAccess::submit(view);
    TEST_CHECK(host.acceptCalls == 1 && host.acceptedResult == subDir);
    view.onClose();
    FileDialog::cancelIfPending();

    // 4. Parent-driven hybrid presentation routing & closed-state invariants on FileDialogPopup.
    {
      auto* fakeWlSurface = reinterpret_cast<wl_surface*>(0x1);
      auto* fakeLayerSurface = reinterpret_cast<zwlr_layer_surface_v1*>(0x2);
      auto* fakeXdgSurface = reinterpret_cast<xdg_surface*>(0x3);
      auto* fakeXdgToplevel = reinterpret_cast<xdg_toplevel*>(0x4);

      TEST_CHECK(!FileDialogPopup::shouldUseToplevelSurface(std::nullopt));
      TEST_CHECK(!FileDialogPopup::shouldUseToplevelSurface(
          LayerPopupParentContext{.surface = fakeWlSurface, .layerSurface = fakeLayerSurface}
      ));
      TEST_CHECK(!FileDialogPopup::shouldUseToplevelSurface(
          LayerPopupParentContext{.surface = fakeWlSurface, .xdgSurface = fakeXdgSurface}
      ));
      TEST_CHECK(
          FileDialogPopup::shouldUseToplevelSurface(
              LayerPopupParentContext{
                  .surface = fakeWlSurface,
                  .xdgSurface = fakeXdgSurface,
                  .xdgToplevel = fakeXdgToplevel,
              }
          )
      );

      FileDialogPopup popup;
      TEST_CHECK(!popup.isOpen());
      TEST_CHECK(!popup.usesToplevelSurface());
      TEST_CHECK(popup.wlSurface() == nullptr);
      TEST_CHECK(!popup.ownsSurface(nullptr));
      TEST_CHECK(!popup.ownsSurface(fakeWlSurface));
      TEST_CHECK(!popup.handlesKeyboardForSurface(nullptr));
      TEST_CHECK(!popup.handlesKeyboardForSurface(fakeWlSurface));
      TEST_CHECK(!popup.openFileDialog());
    }
  });
}
