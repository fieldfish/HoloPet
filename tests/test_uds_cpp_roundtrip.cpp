/**
 * test_uds_cpp_roundtrip.cpp — R4-R1 P4 (D9): 真实 C++ client ↔ Python
 * holopet-agentd 经 Unix Domain Socket 完成 hello/start_turn/content/
 * response_complete/done。
 *
 * 仅 Linux (AF_UNIX); Windows 打印 SKIP 并返回 0 (ctest 记 PASS/SKIP 语义,
 * 验收证据必须注明 Linux 实跑才算 G8)。测试自起 Python agentd (fake,
 * 临时目录 socket), 用与生产同一 AiWorkerSession/握手/发送缓冲/
 * request filter 路径驱动事件。
 *
 * R7_R2 (任务 A): 生命周期收尾 —
 *  - posix_spawnp + posix_spawn_file_actions 启动 python3: 全程无 fork,
 *    不存在 fork-after-thread 或 child 在 exec 前调用不安全操作的问题;
 *  - CleanupGuard 在 mkdtemp 之后、std::thread 创建之前即持有全部自有
 *    资源 (tmpdir/socket/log/pid/status), 线程构造失败也绕过不了清理;
 *  - 所有返回路径逐项删除自有文件并打印每个 remove/rmdir 结果,
 *    随后复检输出零残留报告 [residue] (tmpdir/socket/log/pid/status/child);
 *  - 只对自 spawn 的 PID 做 自退出等待→SIGTERM→有界 waitpid→SIGKILL,
 *    打印 WIFEXITED/WIFSIGNALED 真实结果; 无 pkill -f;
 *  - 故障注入 HOLOPET_TEST_FAULT=after_tmp (exit 20) / after_spawn
 *    (exit 21), 两者都必须短超时非零退出且零残留;
 *  - 无效 agent 目录: 精确 exit 10 + "agent 目录无效" marker;
 *  - 驱动器 tests/test_uds_cpp_lifecycle_driver.py 断言精确退出码/marker/
 *    零残留/三种 cwd/并发两实例; WILL_FAIL 负向已移除。
 */

#include "agent/agent_client.hpp"
#include "ipc/ai_io_loop.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace holopet;

// R7_R2 (A2): 精确退出码 — 驱动器按这些值断言, 不再用 WILL_FAIL 接受
// 任意非零退出。
enum {
    EXIT_OK = 0,
    EXIT_CHECKS_FAILED = 1,
    EXIT_AGENT_DIR_INVALID = 10,
    EXIT_MKDTEMP_FAILED = 11,
    EXIT_SPAWN_FAILED = 12,
    EXIT_THREAD_FAILED = 13,
    EXIT_FAULT_AFTER_TMP = 20,
    EXIT_FAULT_AFTER_SPAWN = 21,
};

static int g_passed = 0, g_failed = 0;
void check(bool c, const std::string& n) {
    if (c) { std::cout << "  PASS: " << n << '\n'; ++g_passed; }
    else   { std::cout << "  FAIL: " << n << '\n'; ++g_failed; }
}

// R7 (A1): 纯函数自检 (Windows/Linux 都执行), 不依赖 cwd。
static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static void self_test_quoting() {
    check(shell_quote("/a b/agent") == "'/a b/agent'", "quote: 空格路径");
    check(shell_quote("a'b") == "'a'\\''b'", "quote: 单引号转义");
    check(shell_quote("中文/目录") == "'中文/目录'", "quote: 非 ASCII");
}

// R7_R1 (A2): agent 目录 — 编译期绝对源码根, 可用 HOLOPET_TEST_AGENT_DIR
// 负向注入 (仅测试二进制读取)。
static std::string agent_dir() {
    const char* override_p = std::getenv("HOLOPET_TEST_AGENT_DIR");
    if (override_p && override_p[0] != '\0') return override_p;
#ifdef HOLOPET_SOURCE_ROOT
    return std::string(HOLOPET_SOURCE_ROOT) + "/agent";
#else
    return "agent";
#endif
}

// 真实退出状态解释 (waitpid status → 文本)。仅 Linux 使用 (MSVC 无 sys/wait.h)。
#ifndef _WIN32
static std::string exit_status_text(int status) {
    if (WIFEXITED(status)) {
        return "exit_code=" + std::to_string(WEXITSTATUS(status));
    }
    if (WIFSIGNALED(status)) {
        return "signal=" + std::to_string(WTERMSIG(status));
    }
    return "status=" + std::to_string(status);
}

// R7_R2 (A2): 自有资源集合 — tmpdir/socket/log/pid/status 全部由本测试
// 创建、拥有并负责清零; 所有权 guard 析构/成功路径都经同一清理函数。
struct CleanupState {
    std::string tmp;
    std::string uds_path;
    std::string ag_log;
    std::string pid_file;
    std::string status_file;
    pid_t child = -1;
    int wait_status = -1;
    bool reaped = false;
    bool cleaned = false;
    std::atomic<bool>* io_running = nullptr;
    std::atomic<bool>* io_started = nullptr;
    std::thread* io = nullptr;
    AiCommandQueue* commands = nullptr;
};

// 只处理自己 spawn 的 PID: 有界自退出等待 (grace) → SIGTERM → 有界
// waitpid → SIGKILL → waitpid; 打印 WIFEXITED/WIFSIGNALED 真实结果。
// 禁止 pkill -f: 其他用户/其他实例的进程不算本测试所有。
static void reap_child(CleanupState& st, int grace_ms) {
    if (st.child <= 0 || st.reaped) return;
    for (int i = 0; i < grace_ms / 100 && !st.reaped; ++i) {
        pid_t r = waitpid(st.child, &st.wait_status, WNOHANG);
        if (r == st.child) { st.reaped = true; break; }
        if (r == -1 && errno == ECHILD) { st.reaped = true; break; }
        usleep(100000);
    }
    if (!st.reaped) {
        std::cout << "[cleanup] 发送 SIGTERM pid=" << st.child << "\n";
        ::kill(st.child, SIGTERM);
        for (int i = 0; i < 30 && !st.reaped; ++i) {
            pid_t r = waitpid(st.child, &st.wait_status, WNOHANG);
            if (r == st.child) { st.reaped = true; break; }
            if (r == -1 && errno == ECHILD) { st.reaped = true; break; }
            usleep(100000);
        }
    }
    if (!st.reaped) {
        std::cout << "[cleanup] SIGTERM 超时, 发送 SIGKILL pid=" << st.child << "\n";
        ::kill(st.child, SIGKILL);
        pid_t r = waitpid(st.child, &st.wait_status, 0);
        st.reaped = (r == st.child || (r == -1 && errno == ECHILD));
    }
    std::cout << "[child] pid=" << st.child << " wait="
              << (st.reaped ? exit_status_text(st.wait_status) : "not-reaped")
              << "\n";
}

// 逐项删除自有文件 (每个 remove 结果可检查), rmdir 目录, 然后 stat 复检
// 输出零残留报告。失败诊断此前已打印到 stdout, 不依赖 /tmp 残留文件。
static void cleanup_all(CleanupState& st, const char* why) {
    std::cout << "[cleanup] begin why=" << why << "\n";
    const std::string* owned[] = {&st.uds_path, &st.ag_log,
                                  &st.pid_file, &st.status_file};
    for (const std::string* p : owned) {
        if (!p->empty()) {
            int rc = ::remove(p->c_str());
            std::cout << "[cleanup] remove " << *p << " rc=" << rc;
            if (rc != 0) std::cout << " errno=" << errno;
            std::cout << "\n";
        }
    }
    int rc = ::rmdir(st.tmp.c_str());
    std::cout << "[cleanup] rmdir " << st.tmp << " rc=" << rc;
    if (rc != 0) std::cout << " errno=" << errno;
    std::cout << "\n";
    auto gone = [](const std::string& p) -> const char* {
        if (p.empty()) return "none";
        struct stat sb;
        return ::stat(p.c_str(), &sb) == 0 ? "present" : "gone";
    };
    const char* child_state = "none";
    if (st.child > 0) {
        child_state = (::kill(st.child, 0) == 0) ? "present" : "gone";
    }
    std::cout << "[residue] tmpdir=" << gone(st.tmp)
              << " socket=" << gone(st.uds_path)
              << " log=" << gone(st.ag_log)
              << " pid=" << gone(st.pid_file)
              << " status=" << gone(st.status_file)
              << " child=" << child_state << "\n";
    st.cleaned = true;
}

// RAII 所有权 guard: 构造时 tmpdir 已存在 (mkdtemp 之后立即构造, 早于
// 任何 std::thread), 任何返回/异常/线程构造失败路径都由析构完成收尾。
struct CleanupGuard {
    CleanupState* st;
    ~CleanupGuard() {
        if (!st || st->cleaned) return;
        if (st->io_started && *st->io_started) {
            if (st->commands) st->commands->push(AiCommand::Shutdown);
            if (st->io_running) st->io_running->store(false);
            if (st->io && st->io->joinable()) st->io->join();
            *st->io_started = false;
        }
        reap_child(*st, 0);   // 收尾路径无 grace: 短超时完成
        cleanup_all(*st, "guard-dtor");
    }
};

// 诊断: agentd 日志尾部 (删除前打印到 stdout, 不依赖残留文件)。
static void print_log_tail(const std::string& path, int n) {
    std::ifstream lf(path);
    if (!lf) {
        std::cout << "    (log 不可读)\n";
        return;
    }
    std::vector<std::string> lines;
    std::string ln;
    while (std::getline(lf, ln)) lines.push_back(ln);
    size_t from = lines.size() > static_cast<size_t>(n)
                      ? lines.size() - static_cast<size_t>(n) : 0;
    for (size_t i = from; i < lines.size(); ++i)
        std::cout << "    " << lines[i] << "\n";
}
#endif

int main() {
    std::cout << "=== test_uds_cpp_roundtrip (V6 R4-R1, R7_R2 lifecycle) ===\n\n";
    self_test_quoting();

    const std::string ag_dir = agent_dir();
    std::cout << "  agent dir (resolved): " << ag_dir << '\n';
    std::cout << "  python3: ";
    std::system("python3 --version");

    // R7_R2 (A2): 无效 agent 目录 — 两个平台都先于 SKIP 检查; 精确
    // exit 10 + "agent 目录无效" marker (驱动器断言, 不是 WILL_FAIL)。
    struct stat st_;
    if (::stat(ag_dir.c_str(), &st_) != 0) {
        std::cout << "  FAIL: agent 目录不存在或不可访问: " << ag_dir
                  << " (errno=" << errno << ")\n";
        std::cout << "[negative] agent 目录无效: " << ag_dir << " → exit "
                  << EXIT_AGENT_DIR_INVALID << "\n";
        std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
                  << " 失败 ===\n";
        return EXIT_AGENT_DIR_INVALID;
    }

#ifdef _WIN32
    std::cout << "  SKIP: AF_UNIX 不可用 (Windows 开发回退 loopback TCP); "
              << "Linux/CI 必须实跑本测试 (R4R1-G8)\n";
    return g_failed == 0 ? EXIT_OK : EXIT_CHECKS_FAILED;
#else
    // 唯一临时目录 (mkdtemp; 可并行, 全部资源隔离)
    char tmpl[] = "/tmp/holopet_uds_cpp_XXXXXX";
    char* tmpdir = mkdtemp(tmpl);
    if (!tmpdir) {
        std::cout << "  FAIL: mkdtemp 失败 errno=" << errno << "\n";
        return EXIT_MKDTEMP_FAILED;
    }
    std::string tmp(tmpdir);
    std::string uds_path = tmp + "/agent.sock";
    std::string ag_log = tmp + "/agentd.log";
    std::string pid_file = tmp + "/agentd.pid";
    std::string status_file = tmp + "/agentd.status";
    std::string cfg = ag_dir + "/../config/agent.example.json";
    std::cout << "  socket: " << uds_path << "\n"
              << "  agentd log: " << ag_log << "\n";

    AiWorkerConfig cfgc;
    cfgc.uds_path = uds_path;
    AiWorkerSession session(cfgc);
    AiCommandQueue commands;
    WorkerEventQueue events;
    std::atomic<bool> io_running{true};
    std::atomic<bool> io_started{false};
    std::thread io;

    CleanupState st;
    st.tmp = tmp;
    st.uds_path = uds_path;
    st.ag_log = ag_log;
    st.pid_file = pid_file;
    st.status_file = status_file;
    st.io_running = &io_running;
    st.io_started = &io_started;
    st.io = &io;
    st.commands = &commands;
    CleanupGuard guard{&st};   // mkdtemp 之后、线程创建之前: 全路径保护

    const char* fault = std::getenv("HOLOPET_TEST_FAULT");
    std::string fault_s = fault ? fault : "";

    // 故障注入 after_tmp: 目录已建、未 spawn → 精确 exit 20 + 零残留
    if (fault_s == "after_tmp") {
        std::cout << "[fault] after_tmp: 临时目录已建未 spawn → exit "
                  << EXIT_FAULT_AFTER_TMP << "\n";
        return EXIT_FAULT_AFTER_TMP;
    }

    // posix_spawnp (无 fork, 推荐路径): 文件动作重定向 log; envp 在父
    // 进程构造 (此时尚未创建任何线程, 无并发环境风险); child 在 exec
    // 前不执行任何自定义代码。
    std::vector<std::string> env_block;
    const std::string py_key = "PYTHONPATH=";
    bool py_replaced = false;
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        if (entry.rfind("PYTHONPATH=", 0) == 0) {
            // 保留既有条目, 追加 agent 目录 (不覆盖用户环境)
            std::string val = entry.substr(py_key.size());
            entry = py_key + (val.empty() ? ag_dir : val + ":" + ag_dir);
            py_replaced = true;
        }
        env_block.push_back(entry);
    }
    if (!py_replaced) env_block.push_back(py_key + ag_dir);
    std::vector<char*> envp;
    envp.reserve(env_block.size() + 1);
    for (auto& s : env_block) envp.push_back(s.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, ag_log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, ag_log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char* const argv[] = {
        const_cast<char*>("python3"), const_cast<char*>("-m"),
        const_cast<char*>("holopet_agentd"), const_cast<char*>("--config"),
        const_cast<char*>(cfg.c_str()), const_cast<char*>("--uds"),
        const_cast<char*>(uds_path.c_str()), const_cast<char*>("--fake"),
        const_cast<char*>("--log-level"), const_cast<char*>("WARNING"),
        nullptr};
    int sprc = posix_spawnp(&st.child, "python3", &fa, nullptr, argv,
                            envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (sprc != 0) {
        std::cout << "  FAIL: posix_spawnp 失败 rc=" << sprc << "\n";
        return EXIT_SPAWN_FAILED;
    }
    {
        std::ofstream pf(pid_file);
        if (pf) pf << "pid=" << st.child << "\n";
    }
    std::cout << "[spawn] agentd pid=" << st.child << "\n";
    check(true, "agentd 子进程启动 (精确 PID 记录)");

    // 故障注入 after_spawn: 子进程已启动 → 先诊断 (存活/日志尾部) 再
    // 精确 exit 21; guard 析构负责 TERM/wait/逐项删除/零残留复检。
    if (fault_s == "after_spawn") {
        std::cout << "[fault] after_spawn: 子进程已启动 pid=" << st.child
                  << " → exit " << EXIT_FAULT_AFTER_SPAWN << "\n";
        std::cout << "  [diagnostic] agentd 日志尾部:\n";
        print_log_tail(ag_log, 20);
        return EXIT_FAULT_AFTER_SPAWN;
    }

    // I/O 线程: guard 已就位, 构造失败也走清理 (tmpdir 已被 guard 持有)
    try {
        io = std::thread([&]() {
            auto now = []() {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            };
            runAiIoLoop(commands, session, io_running, events, now);
        });
        io_started.store(true);
    } catch (const std::exception& e) {
        std::cout << "  FAIL: I/O 线程构造失败: " << e.what() << "\n";
        return EXIT_THREAD_FAILED;
    }

    // 等 hello / connected (真实握手, 无固定长 sleep; 检测子进程提前退出)
    bool got_hello = false;
    auto t0 = std::chrono::steady_clock::now();
    while (!got_hello) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        AgentEvent ev;
        while (events.tryPop(ev)) {
            if (ev.type == AgentEventType::WorkerOnline) got_hello = true;
        }
        pid_t r = waitpid(st.child, &st.wait_status, WNOHANG);
        if (r == st.child) {
            st.reaped = true;
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > 8000) break;
    }
    if (!got_hello) {
        // 诊断 — 路径/Python/存活/真实退出状态/日志尾部 (stdout 保留)
        std::cout << "  [diagnostic] agent dir: " << ag_dir << "\n";
        std::cout << "  [diagnostic] python3: ";
        std::system("python3 --version");
        std::cout << "  [diagnostic] agentd pid: " << st.child
                  << " alive: " << (st.reaped ? "no" : "yes") << "\n";
        std::cout << "  [diagnostic] agentd 真实退出状态: "
                  << (st.reaped ? exit_status_text(st.wait_status)
                                : "(仍在运行)")
                  << "\n";
        std::cout << "  [diagnostic] agentd 日志尾部:\n";
        print_log_tail(ag_log, 20);
    }
    check(got_hello, "UDS 握手 hello 完成 (真实 C++ client ↔ agentd)");

    // 注入 transcript + start_turn + stop_recording
    {
        // 固定 rid → transcript 与 start_turn 同 id (同一发送缓冲路径)
        session.setNextRequestIdForTest("uds-1");
        session.queueTranscriptForTest("uds-1", "你好");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        commands.push(AiCommand::StartTurn);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        commands.push(AiCommand::StopRecording);
    }

    // 收集事件: content / response_complete / done
    bool got_content = false, got_complete = false, got_done = false;
    t0 = std::chrono::steady_clock::now();
    while (!got_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        AgentEvent ev;
        while (events.tryPop(ev)) {
            if (ev.type == AgentEventType::ContentChunk) got_content = true;
            if (ev.type == AgentEventType::ResponseComplete) got_complete = true;
            if (ev.type == AgentEventType::TurnDone) got_done = true;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > 10000) break;
    }
    check(got_content, "UDS 收到 content 分片");
    check(got_complete, "UDS 收到 response_complete");
    check(got_done, "UDS 收到 done");

    // R8_R2 (工作包 D): 生产文字路径 — SubmitTextTurn 经命令队列
    // (IO owner 线程调用 session.submitTextTurn; SDL 主线程不碰 session)。
    // 同时验证 busy: 同轮进行中第二次提交被受控拒绝 (无多余 transcript)。
    {
        commands.push(AiCommand::SubmitTextTurn,
                      std::string("R8R2_TEXT_TURN_MARKER"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        commands.push(AiCommand::SubmitTextTurn, std::string("BUSY_PROBE"));
    }
    int transcript_count = 0;
    bool seen_text_marker = false;
    bool seen_busy_probe = false;
    bool second_done = false;
    t0 = std::chrono::steady_clock::now();
    while (!second_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        AgentEvent ev;
        while (events.tryPop(ev)) {
            if (ev.type == AgentEventType::Transcript) {
                ++transcript_count;
                if (ev.text == "R8R2_TEXT_TURN_MARKER") seen_text_marker = true;
                if (ev.text == "BUSY_PROBE") seen_busy_probe = true;
            }
            if (ev.type == AgentEventType::TurnDone) second_done = true;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > 10000) break;
    }
    check(second_done, "SubmitTextTurn 生产路径: 文字轮完成 (done)");
    check(seen_text_marker, "文字轮 transcript == 提交文本");
    check(!seen_busy_probe,
          "busy: 同轮第二次提交被拒 (无 BUSY_PROBE transcript)");
    check(transcript_count == 1,
          "文字轮恰好 1 次 transcript (无交错/无重复)");

    // 优雅退出: Shutdown → join → 有界自退出等待 → TERM/KILL 兜底
    commands.push(AiCommand::Shutdown);
    io_running.store(false);
    io.join();
    io_started.store(false);
    reap_child(st, 5000);
    check(st.reaped, "agentd 已 wait 并获得真实退出状态");
    {
        std::ofstream sf(status_file);
        if (sf) {
            sf << (st.reaped ? exit_status_text(st.wait_status)
                             : "not-reaped")
               << "\n";
        }
    }

    // 成功路径同样显式清理: 每个 remove/rmdir 结果可检查 + 零残留复检
    cleanup_all(st, "normal-success");

    std::cout << "\n=== 结果: " << g_passed << " 通过, " << g_failed
              << " 失败 ===\n";
    return g_failed == 0 ? EXIT_OK : EXIT_CHECKS_FAILED;
#endif
}
