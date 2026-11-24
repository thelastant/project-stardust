// Self-checking tests for the save data dialog model. Time is injected, so nothing sleeps and no pad provider,
// window or game data is involved.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/threads.h"
#include "hle/hle.h"
#include "hle/save_dialog.h"
#include "hle/save_dialog_model.h"
#include "hle/save_dialog_text.h"
#include "input/keyboard_pad.h"
#include "input/pad_buttons.h"
#include "sshd/ppu_context.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

using sshd::DialogAction;
using sshd::DialogAnswer;
using sshd::DialogInput;
using sshd::DialogKind;
using sshd::PadButton;
using sshd::PadSample;
using sshd::SaveDialogModel;
using Clock = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::seconds(100);

Clock::time_point at(int ms) { return kT0 + std::chrono::milliseconds(ms); }

PadSample held(PadButton b) {
    PadSample s;
    s.buttons = sshd::padButtonBit(b);
    return s;
}

using Actions = std::vector<DialogAction>;

// A DialogInput that has already seen a neutral sample, so every input counts as released.
DialogInput releasedInput() {
    DialogInput input(sshd::dialogButtons());
    input.update(PadSample{}, at(0));
    return input;
}

void testHeldAtConstruction() {
    DialogInput input(sshd::dialogButtons());
    const PadSample circle = held(PadButton::Circle);
    CHECK(input.update(circle, at(0)).empty());
    CHECK(input.update(circle, at(50)).empty());
    CHECK(input.update(PadSample{}, at(100)).empty());
    CHECK((input.update(circle, at(150)) == Actions{DialogAction::Accept}));

    // A direction held since construction never repeats either.
    DialogInput dir(sshd::dialogButtons());
    const PadSample up = held(PadButton::Up);
    bool acted = false;
    for (int ms = 0; ms <= 3000; ms += 5) acted = acted || !dir.update(up, at(ms)).empty();
    CHECK(!acted);
    dir.update(PadSample{}, at(3005));
    CHECK((dir.update(up, at(3010)) == Actions{DialogAction::Up}));
}

void testDpadRepeat() {
    DialogInput input = releasedInput();
    const PadSample down = held(PadButton::Down);
    CHECK((input.update(down, at(10)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(100)).empty());
    CHECK(input.update(down, at(509)).empty());
    CHECK((input.update(down, at(510)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(709)).empty());
    CHECK((input.update(down, at(710)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(909)).empty());
    CHECK((input.update(down, at(910)) == Actions{DialogAction::Down}));

    // Releasing restarts the delay.
    input.update(PadSample{}, at(950));
    CHECK((input.update(down, at(960)) == Actions{DialogAction::Down}));
    CHECK(input.update(down, at(1400)).empty());
    CHECK((input.update(down, at(1460)) == Actions{DialogAction::Down}));
}

void testRepeatAfterStall() {
    DialogInput input = releasedInput();
    const PadSample up = held(PadButton::Up);
    CHECK((input.update(up, at(0)) == Actions{DialogAction::Up}));
    // One late sample gives one action, not a burst of the missed repeats.
    CHECK((input.update(up, at(2000)) == Actions{DialogAction::Up}));
    CHECK(input.update(up, at(2005)).empty());
    CHECK((input.update(up, at(2200)) == Actions{DialogAction::Up}));
}

void testStickThresholds() {
    for (const bool left : {true, false}) {
        const auto sample = [left](std::uint8_t y) {
            PadSample s;
            (left ? s.left_y : s.right_y) = y;
            return s;
        };
        DialogInput input = releasedInput();
        CHECK(input.update(sample(30), at(10)).empty());
        CHECK(input.update(sample(225), at(20)).empty());
        CHECK((input.update(sample(29), at(30)) == Actions{DialogAction::Up}));
        input.update(sample(0x80), at(40));
        CHECK((input.update(sample(226), at(50)) == Actions{DialogAction::Down}));
        // Sticks repeat like the d-pad.
        CHECK(input.update(sample(226), at(549)).empty());
        CHECK((input.update(sample(226), at(550)) == Actions{DialogAction::Down}));
        CHECK(input.update(sample(226), at(749)).empty());
        CHECK((input.update(sample(226), at(750)) == Actions{DialogAction::Down}));
    }
}

void testShoulderButtonsNoRepeat() {
    DialogInput input = releasedInput();
    CHECK((input.update(held(PadButton::L1), at(10)) == Actions{DialogAction::PageUp}));
    CHECK((input.update(held(PadButton::R1), at(20)) == Actions{DialogAction::PageDown}));
    bool acted = false;
    for (int ms = 30; ms <= 3000; ms += 5) acted = acted || !input.update(held(PadButton::R1), at(ms)).empty();
    CHECK(!acted);
}

void testOtherButtonsIgnored() {
    DialogInput input = releasedInput();
    for (const PadButton b : {PadButton::Triangle, PadButton::Square, PadButton::Start, PadButton::Select,
                              PadButton::L2, PadButton::R2, PadButton::Left, PadButton::Right}) {
        CHECK(input.update(held(b), at(10)).empty());
        input.update(PadSample{}, at(20));
    }
}

void testEnterButtonAssign() {
    // Circle is enter by default.
    {
        DialogInput input(sshd::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Circle), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Cross), at(30)) == Actions{DialogAction::Back}));
    }
    sshd::setEnterButtonAssign(sshd::EnterButtonAssign::Cross);
    {
        DialogInput input(sshd::dialogButtons());
        input.update(PadSample{}, at(0));
        CHECK((input.update(held(PadButton::Cross), at(10)) == Actions{DialogAction::Accept}));
        input.update(PadSample{}, at(20));
        CHECK((input.update(held(PadButton::Circle), at(30)) == Actions{DialogAction::Back}));
    }
    sshd::setEnterButtonAssign(sshd::EnterButtonAssign::Circle);
}

void testPadSampleFromState() {
    sshd::PadPortState state =
            sshd::padStateFromButtons(sshd::padButtonBit(PadButton::Up) | sshd::padButtonBit(PadButton::Circle) |
                                      sshd::padButtonBit(PadButton::L1));
    state.button[sshd::kPadButtonAnalogLeftY] = 12;
    state.button[sshd::kPadButtonAnalogRightY] = 250;
    const PadSample sample = sshd::padSampleFromState(state);
    CHECK(sample.buttons == (sshd::padButtonBit(PadButton::Up) | sshd::padButtonBit(PadButton::Circle) |
                             sshd::padButtonBit(PadButton::L1)));
    CHECK(sample.left_y == 12);
    CHECK(sample.right_y == 250);
    const PadSample neutral = sshd::padSampleFromState(sshd::neutralPadState());
    CHECK(neutral.buttons == 0 && neutral.left_y == 0x80 && neutral.right_y == 0x80);
}

std::optional<sshd::DialogResult> stepOne(SaveDialogModel& model, DialogAction action, int ms) {
    const DialogAction actions[] = {action};
    return model.step(actions, at(ms));
}

std::optional<sshd::DialogResult> idle(SaveDialogModel& model, int ms) { return model.step({}, at(ms)); }

void testPageJumpsClamped() {
    SaveDialogModel model(DialogKind::List, 25, 12, at(0));
    stepOne(model, DialogAction::PageDown, 200);
    CHECK(model.selectedRow() == 22);
    stepOne(model, DialogAction::PageDown, 210);
    CHECK(model.selectedRow() == 24);
    stepOne(model, DialogAction::PageUp, 220);
    CHECK(model.selectedRow() == 14);
    stepOne(model, DialogAction::PageUp, 230);
    CHECK(model.selectedRow() == 4);
    stepOne(model, DialogAction::PageUp, 240);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Up, 250);
    CHECK(model.selectedRow() == 0);
}

void testNoWrap() {
    SaveDialogModel model(DialogKind::List, 3, 2, at(0));
    stepOne(model, DialogAction::Down, 200);
    CHECK(model.selectedRow() == 2);
    stepOne(model, DialogAction::Up, 210);
    stepOne(model, DialogAction::Up, 220);
    stepOne(model, DialogAction::Up, 230);
    CHECK(model.selectedRow() == 0);
}

void testFadeInIgnoresInput() {
    SaveDialogModel model(DialogKind::List, 5, 0, at(0));
    stepOne(model, DialogAction::Down, 0);
    stepOne(model, DialogAction::Down, 149);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Down, 150);
    CHECK(model.selectedRow() == 1);

    SaveDialogModel accept(DialogKind::List, 5, 3, at(0));
    stepOne(accept, DialogAction::Accept, 149);
    CHECK(!idle(accept, 400));  // never closed: the Accept at 149 was dropped
    CHECK(accept.opacity(at(400)) == 1.0f);
}

void testResultAfterFadeOut() {
    SaveDialogModel model(DialogKind::List, 5, 3, at(0));
    CHECK(!stepOne(model, DialogAction::Accept, 200));
    CHECK(!idle(model, 349));
    const auto result = idle(model, 350);
    CHECK(result.has_value());
    if (result) {
        CHECK(result->answer == DialogAnswer::Row);
        CHECK(result->row == 3);
    }

    // Input after the close does not change the result.
    SaveDialogModel closing(DialogKind::List, 5, 3, at(0));
    stepOne(closing, DialogAction::Back, 200);
    stepOne(closing, DialogAction::Accept, 250);
    const auto back = idle(closing, 400);
    CHECK(back.has_value() && back->answer == DialogAnswer::Back && back->row == -1);
}

void testFirstCloseWinsWithinOneStep() {
    SaveDialogModel model(DialogKind::List, 5, 1, at(0));
    const DialogAction actions[] = {DialogAction::Back, DialogAction::Accept};
    model.step(actions, at(200));
    const auto result = idle(model, 350);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testEmptyListIgnoresAccept() {
    SaveDialogModel model(DialogKind::List, 0, 4, at(0));
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Accept, 200);
    CHECK(!idle(model, 500));
    CHECK(model.opacity(at(500)) == 1.0f);
    stepOne(model, DialogAction::Down, 510);
    CHECK(model.selectedRow() == 0);
    stepOne(model, DialogAction::Back, 520);
    const auto result = idle(model, 700);
    CHECK(result.has_value() && result->answer == DialogAnswer::Back);
}

void testOkIgnoresBack() {
    SaveDialogModel model(DialogKind::Ok, 0, 0, at(0));
    stepOne(model, DialogAction::Back, 200);
    CHECK(!idle(model, 500));
    stepOne(model, DialogAction::Accept, 510);
    CHECK(!idle(model, 600));
    const auto result = idle(model, 660);
    CHECK(result.has_value() && result->answer == DialogAnswer::Ok);
}

void testYesNo() {
    SaveDialogModel yes(DialogKind::YesNo, 0, 0, at(0));
    stepOne(yes, DialogAction::Accept, 200);
    const auto y = idle(yes, 350);
    CHECK(y.has_value() && y->answer == DialogAnswer::Yes);

    SaveDialogModel no(DialogKind::YesNo, 0, 0, at(0));
    stepOne(no, DialogAction::Back, 200);
    const auto n = idle(no, 350);
    CHECK(n.has_value() && n->answer == DialogAnswer::No);
}

void testInitialRowAndScroll() {
    // Centred on open, clamped at both ends.
    CHECK(SaveDialogModel(DialogKind::List, 20, 0, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 2, at(0)).firstVisibleRow() == 0);
    CHECK(SaveDialogModel(DialogKind::List, 20, 10, at(0)).firstVisibleRow() == 8);
    CHECK(SaveDialogModel(DialogKind::List, 20, 19, at(0)).firstVisibleRow() == 16);
    CHECK(SaveDialogModel(DialogKind::List, 3, 1, at(0)).firstVisibleRow() == 0);
    // initial_row is clamped into the list.
    CHECK(SaveDialogModel(DialogKind::List, 4, 99, at(0)).selectedRow() == 3);
    CHECK(SaveDialogModel(DialogKind::List, 4, -5, at(0)).selectedRow() == 0);
}

void testScrollKeepsSelectionVisible() {
    SaveDialogModel model(DialogKind::List, 12, 0, at(0));
    int ms = 200;
    for (int i = 0; i < 14; i++, ms += 10) {
        stepOne(model, DialogAction::Down, ms);
        CHECK(model.selectedRow() >= model.firstVisibleRow());
        CHECK(model.selectedRow() < model.firstVisibleRow() + SaveDialogModel::kVisibleRows);
    }
    CHECK(model.selectedRow() == 11);
    CHECK(model.firstVisibleRow() == 8);
    // Moving up inside the window does not scroll; leaving it scrolls by the minimum.
    for (int i = 0; i < 4; i++, ms += 10) stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 7);
    CHECK(model.firstVisibleRow() == 7);
    stepOne(model, DialogAction::Up, ms);
    CHECK(model.selectedRow() == 6);
    CHECK(model.firstVisibleRow() == 6);
    ms += 10;
    stepOne(model, DialogAction::PageUp, ms);
    CHECK(model.selectedRow() == 0);
    CHECK(model.firstVisibleRow() == 0);
    ms += 10;
    stepOne(model, DialogAction::PageDown, ms);
    CHECK(model.selectedRow() == 10);
    CHECK(model.firstVisibleRow() == 7);
}

void testOpacity() {
    SaveDialogModel model(DialogKind::List, 3, 0, at(0));
    CHECK(model.opacity(at(0)) == 0.0f);
    CHECK(model.opacity(at(75)) > 0.49f && model.opacity(at(75)) < 0.51f);
    CHECK(model.opacity(at(150)) == 1.0f);
    CHECK(model.opacity(at(1000)) == 1.0f);
    stepOne(model, DialogAction::Back, 1000);
    CHECK(model.opacity(at(1000)) == 1.0f);
    CHECK(model.opacity(at(1075)) > 0.49f && model.opacity(at(1075)) < 0.51f);
    CHECK(model.opacity(at(1150)) == 0.0f);
    CHECK(model.opacity(at(2000)) == 0.0f);
}

void testFormatSaveSize() {
    CHECK(sshd::formatSaveSize(0) == "0 KB");
    CHECK(sshd::formatSaveSize(1) == "1 KB");
    CHECK(sshd::formatSaveSize(1000) == "1 KB");
    CHECK(sshd::formatSaveSize(1001) == "2 KB");
    CHECK(sshd::formatSaveSize(1000000) == "1000 KB");
    CHECK(sshd::formatSaveSize(1000001) == "2 MB");
    CHECK(sshd::formatSaveSize(2000000) == "2 MB");
    CHECK(sshd::formatSaveSize(2000001) == "3 MB");
}

std::tm fixedTime() {
    std::tm t{};
    t.tm_year = 2015 - 1900;
    t.tm_mon = 7;
    t.tm_mday = 21;
    t.tm_hour = 13;
    t.tm_min = 52;
    return t;
}

void testFormatSaveDate() {
    CHECK(sshd::formatSaveDate(fixedTime()) == "2015/08/21  13:52");
    std::tm early{};
    early.tm_year = 2001 - 1900;
    early.tm_mon = 0;
    early.tm_mday = 2;
    early.tm_hour = 3;
    early.tm_min = 4;
    CHECK(sshd::formatSaveDate(early) == "2001/01/02  03:04");
    CHECK(sshd::saveRowInfo(fixedTime(), 1001) == "2015/08/21  13:52   2 KB");
}

void testSaveConfirmText() {
    CHECK(sshd::saveConfirmText("Q?", "T", "S", "D", "Z", "detail") == "Q?\n\nT\nS\nD\nZ\n\ndetail");
    // Empty fields keep their lines.
    CHECK(sshd::saveConfirmText("Q?", "T", "", "D", "Z", "") == "Q?\n\nT\n\nD\nZ\n\n");
    CHECK(sshd::saveConfirmText(sshd::kConfirmOverwrite, "", "", "", "", "") ==
          std::string(sshd::kConfirmOverwrite) + "\n\n\n\n\n\n\n");
}

void testSaveErrorText() {
    const auto no_space = sshd::saveErrorText(-1, 12, "");
    CHECK(no_space.has_value() && *no_space == "空き容量が足りません。\n\n必要な空き容量：12 KB");
    const auto failure = sshd::saveErrorText(-2, 0, "");
    CHECK(failure.has_value() && *failure == "セーブまたはロードに失敗しました。");
    const auto broken = sshd::saveErrorText(-3, 0, "");
    CHECK(broken.has_value() && *broken == "セーブデータが壊れています。");
    const auto no_data = sshd::saveErrorText(-4, 0, "");
    CHECK(no_data.has_value() && *no_data == "セーブデータが見つかりません。");
    const auto invalid = sshd::saveErrorText(-5, 0, "custom message");
    CHECK(invalid.has_value() && *invalid == "custom message");
    const auto invalid_empty = sshd::saveErrorText(-5, 0, "");
    CHECK(invalid_empty.has_value() && invalid_empty->empty());
    CHECK(!sshd::saveErrorText(3, 0, "ignored").has_value());
    CHECK(!sshd::saveErrorText(0, 0, "ignored").has_value());
}

std::shared_ptr<sshd::SaveDialogContent> contentWithRows(int rows, DialogKind kind = DialogKind::List) {
    auto content = std::make_shared<sshd::SaveDialogContent>();
    content->kind = kind;
    content->rows.resize(static_cast<std::size_t>(rows));
    return content;
}

void testAutoDriver() {
    auto driver = sshd::makeAutoSaveDialogDriver(sshd::SaveDialogPolicy::Auto);
    CHECK(!driver->interactive());

    CHECK(driver->showList({contentWithRows(3), 1}) == std::optional<std::int32_t>(1));
    CHECK(driver->showList({contentWithRows(3), 9}) == std::optional<std::int32_t>(2));
    CHECK(driver->showList({contentWithRows(3), -4}) == std::optional<std::int32_t>(0));
    CHECK(!driver->showList({contentWithRows(0), 0}).has_value());

    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == sshd::SaveMessageResult::Yes);
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == sshd::SaveMessageResult::Ok);
}

void testCancelDriver() {
    auto driver = sshd::makeAutoSaveDialogDriver(sshd::SaveDialogPolicy::Cancel);
    CHECK(!driver->interactive());
    CHECK(!driver->showList({contentWithRows(3), 1}).has_value());
    CHECK(!driver->showList({contentWithRows(0), 0}).has_value());
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::YesNo)) == sshd::SaveMessageResult::No);
    CHECK(driver->showMessage(contentWithRows(0, DialogKind::Ok)) == sshd::SaveMessageResult::Ok);
}

void testBoard() {
    sshd::SaveDialogBoard board;
    CHECK(board.read().content == nullptr);
    CHECK(board.drawer() == sshd::SaveDialogDrawer::None);

    auto content = contentWithRows(2);
    sshd::SaveDialogSnapshot snapshot;
    snapshot.content = content;
    snapshot.selected_row = 1;
    snapshot.first_visible_row = 1;
    snapshot.opacity = 0.5f;
    board.publish(snapshot);
    const auto first = board.read();
    CHECK(first.content == content);
    CHECK(first.selected_row == 1);
    CHECK(first.first_visible_row == 1);
    CHECK(first.opacity == 0.5f);

    board.publish(snapshot);
    const auto second = board.read();
    CHECK(second.serial > first.serial);

    board.clear();
    const auto cleared = board.read();
    CHECK(cleared.content == nullptr);
    CHECK(cleared.serial > second.serial);

    board.setDrawer(sshd::SaveDialogDrawer::Ready);
    CHECK(board.drawer() == sshd::SaveDialogDrawer::Ready);
    board.setDrawer(sshd::SaveDialogDrawer::Failed);
    CHECK(board.drawer() == sshd::SaveDialogDrawer::Failed);
    board.clear();
    CHECK(board.drawer() == sshd::SaveDialogDrawer::Failed);
}

void testDriverRegistry() {
    sshd::resetSaveDialog();
    CHECK(!sshd::saveDialogDriver().interactive());
    CHECK(sshd::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == sshd::SaveMessageResult::Yes);
    CHECK(!sshd::saveDialogOpen());

    sshd::setSaveDialogDriver(sshd::makeAutoSaveDialogDriver(sshd::SaveDialogPolicy::Cancel));
    CHECK(sshd::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == sshd::SaveMessageResult::No);
    sshd::setSaveDialogDriver(nullptr);
    CHECK(sshd::saveDialogDriver().showMessage(contentWithRows(0, DialogKind::YesNo)) == sshd::SaveMessageResult::Yes);

    sshd::saveDialogBoard().setDrawer(sshd::SaveDialogDrawer::Ready);
    sshd::SaveDialogSnapshot snapshot;
    snapshot.content = contentWithRows(1);
    sshd::saveDialogBoard().publish(snapshot);
    sshd::setSaveDialogDriver(sshd::makeInteractiveSaveDialogDriver(sshd::saveDialogBoard()));
    CHECK(sshd::saveDialogDriver().interactive());
    sshd::resetSaveDialog();
    CHECK(!sshd::saveDialogDriver().interactive());
    CHECK(sshd::saveDialogBoard().read().content == nullptr);
    CHECK(sshd::saveDialogBoard().drawer() == sshd::SaveDialogDrawer::None);
}

// Every wait below has a deadline. When one expires the dialog is told its overlay failed, which makes the driver
// leave its loop, so a broken driver fails checks instead of hanging the run.
constexpr std::chrono::milliseconds kDeadline{5000};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = kDeadline) {
    const auto deadline = Clock::now() + timeout;
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A host thread that holds the kernel lock (and in the split PPU modes a PPU slot) while it runs `body`, like a guest
// thread inside a handler.
class GuestRun {
public:
    GuestRun(sshd::SaveDialogBoard& board, std::function<void()> body, bool use_slot = false) : board_(board) {
        thread_ = std::thread([this, body = std::move(body), use_slot] {
            if (use_slot) sshd::ppuSlots().acquire();
            sshd::guestLock().lock();
            body();
            sshd::guestLock().unlock();
            if (use_slot) sshd::ppuSlots().release();
            finished_at_ = Clock::now();
            done_.store(true);
        });
    }
    ~GuestRun() { join(); }
    GuestRun(const GuestRun&) = delete;
    GuestRun& operator=(const GuestRun&) = delete;

    // False when the body did not end within `timeout`.
    bool join(std::chrono::milliseconds timeout = kDeadline) {
        const bool finished = waitFor([this] { return done_.load(); }, timeout);
        if (!finished) board_.setDrawer(sshd::SaveDialogDrawer::Failed);
        if (thread_.joinable()) thread_.join();
        return finished;
    }

    // When the body ended; valid after join() returned true.
    Clock::time_point finishedAt() const { return finished_at_; }

private:
    sshd::SaveDialogBoard& board_;
    Clock::time_point finished_at_{};
    std::atomic<bool> done_{false};
    std::thread thread_;
};

// Runs `fn` on a thread that holds the kernel lock, as the test's own view into state that handlers guard. In the split
// modes it also needs a PPU slot, so a driver that kept its slot or the lock during the wait leaves this call waiting
// until the join deadline, which then fails the check (and makes the driver give up) instead of passing. A driver that
// ignored the drawer as well would still hang the join.
bool underGuestLock(sshd::SaveDialogBoard& board, std::function<void()> fn) {
    GuestRun run(board, std::move(fn), sshd::splitPpuMode());
    return run.join();
}

struct DialogFixture {
    std::shared_ptr<sshd::KeyboardPadState> pad = std::make_shared<sshd::KeyboardPadState>();
    sshd::SaveDialogBoard board;
    std::unique_ptr<sshd::SaveDialogDriver> driver;

    explicit DialogFixture(bool drawer_ready = true) {
        sshd::resetCellSysutil();
        sshd::resetCellPad();
        sshd::resetSaveDialog();
        sshd::setPadProvider(sshd::makeKeyboardPadProvider(pad));
        if (drawer_ready) board.setDrawer(sshd::SaveDialogDrawer::Ready);
        driver = sshd::makeInteractiveSaveDialogDriver(board);
    }
    ~DialogFixture() {
        sshd::resetCellMsgDialog();
        sshd::setPadProvider(nullptr);
        sshd::resetCellPad();
        sshd::resetSaveDialog();
        sshd::resetCellSysutil();
    }

    void press(PadButton button, bool down) {
        pad->setButtons(sshd::InputSource::Keyboard, sshd::padButtonBit(button), down, Clock::now());
    }
    bool waitForDialog() {
        return waitFor([this] { return board.read().content != nullptr; });
    }
    // Input is dropped during the fade-in, so presses wait for it to end.
    bool waitForFadeIn() {
        return waitFor([this] {
            const auto snapshot = board.read();
            return snapshot.content && snapshot.opacity >= 0.999f;
        });
    }
};

// What the test thread sees of the system state while no dialog code runs.
struct SystemView {
    bool intercepted = false;
    bool dialog_open = false;
    std::int32_t begin = 0;  // queueSysutilEvent(DRAWING_BEGIN)
};

SystemView viewSystem(DialogFixture& f, bool probe_begin) {
    SystemView view;
    CHECK(underGuestLock(f.board, [&] {
        view.intercepted = sshd::padIntercepted();
        view.dialog_open = sshd::saveDialogOpen();
        if (probe_begin) view.begin = sshd::queueSysutilEvent(sshd::kSysutilDrawingBegin, 0);
    }));
    return view;
}

// DRAWING_END for a BEGIN the test sent itself or probed successfully.
std::int32_t sendEnd(DialogFixture& f) {
    std::int32_t result = 0;
    CHECK(underGuestLock(f.board, [&] { result = sshd::queueSysutilEvent(sshd::kSysutilDrawingEnd, 0); }));
    return result;
}

constexpr auto kBusy = static_cast<std::int32_t>(sshd::kSysutilErrorBusy);

// The interactive list: published, pad taken over, pad given back, DRAWING_BEGIN held and released. In the split
// modes the dialog thread also holds a PPU slot, which the wait has to give up as well as the kernel lock.
void testInteractiveList(sshd::PpuThreadsMode mode) {
    sshd::setPpuThreadsMode(mode);
    {
        DialogFixture f;
        std::optional<std::int32_t> row = 99;
        GuestRun run(
                f.board, [&] { row = f.driver->showList({contentWithRows(3), 1}); },
                mode != sshd::PpuThreadsMode::Legacy);

        CHECK(f.waitForDialog());
        const auto first = f.board.read();
        CHECK(first.content && first.content->kind == DialogKind::List && first.content->id != 0);
        CHECK(first.selected_row == 1);

        // The kernel lock is free during the wait: a second BEGIN is refused and the pad is intercepted.
        const SystemView during = viewSystem(f, true);
        CHECK(during.begin == kBusy);
        CHECK(during.intercepted);
        CHECK(during.dialog_open);

        CHECK(f.waitForFadeIn());
        f.press(PadButton::Down, true);
        CHECK(waitFor([&] { return f.board.read().selected_row == 2; }));
        f.press(PadButton::Down, false);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);

        CHECK(row == std::optional<std::int32_t>(2));
        CHECK(f.board.read().content == nullptr);
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted);
        CHECK(!after.dialog_open);
        CHECK(after.begin == 0);  // the dialog's END went out, so BEGIN is accepted again
        CHECK(sendEnd(f) == 0);
    }
    sshd::setPpuThreadsMode(sshd::PpuThreadsMode::Legacy);
}

void testInteractiveMessage() {
    struct Case {
        DialogKind kind;
        PadButton button;
        sshd::SaveMessageResult expected;
    };
    const Case cases[] = {
            {DialogKind::YesNo, PadButton::Circle, sshd::SaveMessageResult::Yes},
            {DialogKind::YesNo, PadButton::Cross, sshd::SaveMessageResult::No},
            {DialogKind::Ok, PadButton::Circle, sshd::SaveMessageResult::Ok},
    };
    for (const Case& c : cases) {
        DialogFixture f;
        sshd::SaveMessageResult result = sshd::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, c.kind)); });
        CHECK(f.waitForFadeIn());
        const auto shown = f.board.read();
        CHECK(shown.content && shown.content->kind == c.kind);
        f.press(c.button, true);
        CHECK(run.join());
        f.press(c.button, false);
        CHECK(result == c.expected);
        CHECK(sendEnd(f) == -1);  // END already went out
    }
}

// Without anything to draw the dialog is answered as Cancel at once and touches neither the pad nor DRAWING_BEGIN.
void testNoDrawerAnswersCancel() {
    for (const sshd::SaveDialogDrawer drawer : {sshd::SaveDialogDrawer::None, sshd::SaveDialogDrawer::Failed}) {
        DialogFixture f(false);
        f.board.setDrawer(drawer);
        std::optional<std::int32_t> row = 99;
        sshd::SaveMessageResult yes_no = sshd::SaveMessageResult::Failed;
        sshd::SaveMessageResult ok = sshd::SaveMessageResult::Failed;
        SystemView after;
        GuestRun run(f.board, [&] {
            row = f.driver->showList({contentWithRows(3), 1});
            yes_no = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo));
            ok = f.driver->showMessage(contentWithRows(0, DialogKind::Ok));
            after.intercepted = sshd::padIntercepted();
            after.dialog_open = sshd::saveDialogOpen();
            after.begin = sshd::queueSysutilEvent(sshd::kSysutilDrawingBegin, 0);
        });
        CHECK(run.join());
        CHECK(!row.has_value());
        CHECK(yes_no == sshd::SaveMessageResult::No);
        CHECK(ok == sshd::SaveMessageResult::Ok);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == 0);
        CHECK(f.board.read().content == nullptr);
    }
}

// The overlay can fail while a dialog waits. The driver must notice within a few ticks, answer as Cancel and still
// release the pad and DRAWING_BEGIN.
void testDrawerFailsDuringWait() {
    for (const DialogKind kind : {DialogKind::List, DialogKind::YesNo}) {
        DialogFixture f;
        std::optional<std::int32_t> row = 99;
        sshd::SaveMessageResult message = sshd::SaveMessageResult::Failed;
        GuestRun run(f.board, [&] {
            if (kind == DialogKind::List) {
                row = f.driver->showList({contentWithRows(2), 0});
            } else {
                message = f.driver->showMessage(contentWithRows(0, kind));
            }
        });
        CHECK(f.waitForDialog());
        CHECK(viewSystem(f, false).intercepted);
        const auto failed_at = Clock::now();
        f.board.setDrawer(sshd::SaveDialogDrawer::Failed);
        const std::uint64_t serial_at_failure = f.board.read().serial;
        CHECK(run.join());
        // The driver notices on its next wake: at most one publish already in flight, then the clear. Counting
        // publishes keeps this independent of machine load; the wall-time bound only guards against a hang.
        CHECK(f.board.read().serial <= serial_at_failure + 2);
        CHECK(run.finishedAt() - failed_at < std::chrono::milliseconds(1000));
        if (kind == DialogKind::List) {
            CHECK(!row.has_value());
        } else {
            CHECK(message == sshd::SaveMessageResult::No);
        }
        CHECK(f.board.read().content == nullptr);
        const SystemView after = viewSystem(f, false);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(sendEnd(f) == -1);  // END was sent
    }
}

// Another system dialog holds DRAWING_BEGIN. A message cannot be shown; a list opens anyway and leaves that BEGIN
// alone (description 2.1).
void testBeginOutstanding() {
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(sshd::queueSysutilEvent(sshd::kSysutilDrawingBegin, 0) == 0); }));
        sshd::SaveMessageResult result = sshd::SaveMessageResult::Ok;
        GuestRun run(f.board, [&] { result = f.driver->showMessage(contentWithRows(0, DialogKind::YesNo)); });
        CHECK(run.join());
        CHECK(result == sshd::SaveMessageResult::Failed);
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // still the other dialog's
        CHECK(f.board.read().content == nullptr);
        CHECK(sendEnd(f) == 0);
    }
    {
        DialogFixture f;
        CHECK(underGuestLock(f.board, [&] { CHECK(sshd::queueSysutilEvent(sshd::kSysutilDrawingBegin, 0) == 0); }));
        std::optional<std::int32_t> row = 99;
        GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(2), 0}); });
        CHECK(f.waitForFadeIn());
        const SystemView during = viewSystem(f, false);
        CHECK(during.intercepted && during.dialog_open);
        f.press(PadButton::Circle, true);
        CHECK(run.join());
        f.press(PadButton::Circle, false);
        CHECK(row == std::optional<std::int32_t>(0));
        const SystemView after = viewSystem(f, true);
        CHECK(!after.intercepted && !after.dialog_open);
        CHECK(after.begin == kBusy);  // no END was sent for a BEGIN the dialog did not make
        CHECK(sendEnd(f) == 0);
    }
}

// The game's own cellMsgDialogOpen2 is refused while a save dialog is open, and accepted once it closed.
void testMsgDialogBusy() {
    DialogFixture f;
    std::vector<std::uint8_t> memory(0x400);
    std::memcpy(memory.data() + 0x100, "hello", 6);
    const sshd::HleEntry* open2 = nullptr;
    for (const sshd::HleEntry& entry : sshd::cellMsgDialogHandlers()) {
        if (std::string_view(entry.name) == "cellMsgDialogOpen2") open2 = &entry;
    }
    CHECK(open2 != nullptr);
    if (open2 == nullptr) return;
    auto callOpen2 = [&] {
        SshdPpuContext ctx{};
        ctx.gpr[3] = 2u << 4;  // OK button
        ctx.gpr[4] = 0x100;
        open2->handler(&ctx, memory.data());
        return static_cast<std::uint32_t>(ctx.gpr[3]);
    };

    std::optional<std::int32_t> row = 99;
    GuestRun run(f.board, [&] { row = f.driver->showList({contentWithRows(1), 0}); });
    CHECK(f.waitForFadeIn());

    // BEGIN is outstanding too, so the result alone cannot tell the two refusals apart; the DRAWING_BEGIN error
    // line only appears when the handler got as far as asking for it.
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    sshd::setLogSink([&](std::string_view line) {
        std::lock_guard lock(lines_mutex);
        lines.emplace_back(line);
    });
    std::uint32_t during = 0;
    CHECK(underGuestLock(f.board, [&] { during = callOpen2(); }));
    sshd::setLogSink({});
    CHECK(during == sshd::kSysutilErrorBusy);
    bool asked_for_begin = false;
    for (const std::string& line : lines) asked_for_begin |= line.find("second DRAWING_BEGIN") != std::string::npos;
    CHECK(!asked_for_begin);

    f.press(PadButton::Circle, true);
    CHECK(run.join());
    f.press(PadButton::Circle, false);
    std::uint32_t after = 0xffffffff;
    CHECK(underGuestLock(f.board, [&] { after = callOpen2(); }));
    CHECK(after == 0);
}

}  // namespace

int main() {
    testHeldAtConstruction();
    testDpadRepeat();
    testRepeatAfterStall();
    testStickThresholds();
    testShoulderButtonsNoRepeat();
    testOtherButtonsIgnored();
    testEnterButtonAssign();
    testPadSampleFromState();
    testPageJumpsClamped();
    testNoWrap();
    testFadeInIgnoresInput();
    testResultAfterFadeOut();
    testFirstCloseWinsWithinOneStep();
    testEmptyListIgnoresAccept();
    testOkIgnoresBack();
    testYesNo();
    testInitialRowAndScroll();
    testScrollKeepsSelectionVisible();
    testOpacity();
    testFormatSaveSize();
    testFormatSaveDate();
    testSaveConfirmText();
    testSaveErrorText();
    testAutoDriver();
    testCancelDriver();
    testBoard();
    testDriverRegistry();
    testInteractiveList(sshd::PpuThreadsMode::Legacy);
    testInteractiveList(sshd::PpuThreadsMode::Split1);
    testInteractiveList(sshd::PpuThreadsMode::Split2);
    testInteractiveMessage();
    testNoDrawerAnswersCancel();
    testDrawerFailsDuringWait();
    testBeginOutstanding();
    testMsgDialogBusy();
    std::printf("save dialog tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
