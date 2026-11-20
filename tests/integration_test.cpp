#include "../src/claude_config.cpp"
#include "../src/placement.h"
#include <cassert>
#include <cstdio>

int main() {
    const RECT taskbar{0, 1000, 1920, 1080};
    assert(widget::EdgeBeforePeer(1720, taskbar, {1580, 1000, 1718, 1080}, false) == 1580);
    assert(widget::EdgeBeforePeer(200, taskbar, {202, 1000, 338, 1080}, true) == 338);
    assert(widget::EdgeBeforePeer(1720, taskbar, {1580, 900, 1718, 980}, false) == 1720);

    const std::wstring current = L"C:\\Programs\\AI Usage Monitor\\AIUsageMonitor.exe";
    const std::wstring old = L"C:\\User's Programs\\Wireless Battery\\WirelessBattery.exe";
    const std::string existing = R"({"theme":"auto","nested":{"example":[true,false]}})";
    auto contents = existing;
    using ai::settings::Update;
    assert(Update(contents, current, true) == 0);
    auto configured = contents;
    assert(Update(contents, current, true) == 0 && contents == configured);
    assert(Update(contents, current, false) == 0);
    assert(contents.find("statusLine") == std::string::npos && contents.find(R"("theme":"auto")") != std::string::npos);

    auto oldScript = ai::settings::Pipe(L"$input", old);
    auto command = ai::settings::Ascii(ai::settings::Encode(oldScript));
    auto oldSettings = "{\"statusLine\":{\"type\":\"command\",\"command\":\"" + command +
                       "\",\"padding\":2},\"theme\":\"auto\"}";
    contents = oldSettings;
    assert(Update(contents, current, true) == 0 && contents != oldSettings);
    assert(contents.find("\"padding\":2") != std::string::npos);
    assert(Update(contents, current, false) == 0 && contents == oldSettings);

    const std::string custom = R"({"statusLine":{"type":"command","command":"starship prompt"}})";
    contents = custom;
    assert(Update(contents, current, true) == 2 && contents == custom);
    assert(Update(contents, current, false) == 2 && contents == custom);
    contents = "{bad JSON";
    assert(Update(contents, current, true) == 3 && contents == "{bad JSON");

    using ai::settings::PriorTrust;
    using ai::settings::UpdateTrust;
    auto folder = ai::settings::WorkspaceKey(L"C:\\Users\\Test\\AIUsageMonitor\\ClaudeCode");
    assert(folder == "C:/Users/Test/AIUsageMonitor/ClaudeCode");
    auto before = std::string(R"({"projects":{"C:/Projects/Existing":{"hasTrustDialogAccepted":true},")") +
        folder + R"(":{"lastSessionId":"example"}},"numStartups":2})";
    contents = before;
    auto prior = PriorTrust::Unmanaged;
    assert(UpdateTrust(contents, folder, true, prior) == 0 && prior == PriorTrust::Missing);
    assert(contents.find(R"("lastSessionId":"example","hasTrustDialogAccepted":true)") != std::string::npos);
    auto newTrust = contents;
    auto manual = PriorTrust::Unmanaged;
    assert(UpdateTrust(contents, folder, true, manual) == 0 && contents == newTrust && manual == PriorTrust::Unmanaged);
    assert(UpdateTrust(contents, folder, false, manual) == 0 && contents == newTrust);
    assert(UpdateTrust(contents, folder, false, prior) == 0 && contents == before);

    contents = "{\"projects\":{\"" + folder + "\":{\"hasTrustDialogAccepted\":false}}}";
    before = contents;
    assert(UpdateTrust(contents, folder, true, prior) == 0 && prior == PriorTrust::Denied);
    assert(UpdateTrust(contents, folder, false, prior) == 0 && contents == before);

    contents = R"({"theme":"auto"})";
    assert(UpdateTrust(contents, folder, true, prior) == 0 && prior == PriorTrust::Missing);
    assert(UpdateTrust(contents, folder, false, prior) == 0 &&
           contents == "{\"theme\":\"auto\",\"projects\":{\"" + folder + "\":{}}}");
    contents = R"({"projects":false})";
    assert(UpdateTrust(contents, folder, true, prior) == 3 && contents == R"({"projects":false})");
    puts("Side-by-side placement and Claude settings tests passed");
}
