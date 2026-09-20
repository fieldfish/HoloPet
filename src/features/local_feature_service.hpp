#pragma once
/**
 * local_feature_service.hpp — R8_R4 统一日常功能服务层 (纯 C++, 无 SDL)。
 *
 * 单一权威服务: C++ 菜单与 agentd 工具请求共用同一实例 (UI_VOICE_SHARED_SERVICE)。
 * 所有时间为注入值: tick(now_mono_ms, now_wall_ms); 持久化路径注入 (测试用临时目录)。
 * 上限: 定时器 ≤8 / 闹钟 ≤16 / 便签 ≤100 (每条 ≤500 Unicode 字符)。
 * 幂等由 (request_id, tool_call_id) 在上层协议保证; 本层 id 单调唯一。
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace holopet {

struct FeatureError {
    bool ok = true;
    std::string code;          // limit_exceeded|not_found|invalid_arg|persist_error|corrupt_recovered
};

struct TimerItem {
    std::string id;
    std::string label;
    int64_t duration_ms = 0;
    int64_t ends_mono_ms = 0;   // 单调到期 (系统时间变化不跳变)
    int64_t ends_wall_ms = 0;   // 持久化的目标墙钟 (重启后重算单调到期)
    bool fired = false;
};

struct AlarmItem {
    std::string id;
    std::string label;
    int64_t hour = 0, minute = 0;          // 本地时区 24h
    int repeat = 0;                        // 0=一次性 1=每日 2=工作日
    bool enabled = true;
    bool fired_once = false;               // 一次性触发后自动失效
    int64_t next_fire_wall_ms = 0;         // 下一次触发墙钟 (本地解释)
};

struct NoteItem {
    std::string id;
    std::string text;
    int64_t created_wall_ms = 0;
};

struct FireEvent {
    std::string kind;          // timer|alarm
    std::string id;
};

enum class DisplayMode { Pet, Clock };

class LocalFeatureService {
public:
    explicit LocalFeatureService(std::filesystem::path store_path = {})
        : store_path_(std::move(store_path)) {}

    // ---- 定时器 ----
    FeatureError createTimer(int64_t duration_ms, const std::string& label,
                             int64_t now_mono_ms, int64_t now_wall_ms = 0,
                             std::string* out_id = nullptr) {
        FeatureError e;
        if (duration_ms <= 0) { e.ok = false; e.code = "invalid_arg"; return e; }
        if (timers_.size() >= kMaxTimers) { e.ok = false; e.code = "limit_exceeded"; return e; }
        TimerItem t;
        t.id = nextId("t");
        t.label = label;
        t.duration_ms = duration_ms;
        t.ends_mono_ms = now_mono_ms + duration_ms;
        t.ends_wall_ms = now_wall_ms + duration_ms;
        timers_.push_back(t);
        if (out_id) *out_id = t.id;
        return e;
    }

    std::vector<TimerItem> listTimers() const { return timers_; }

    FeatureError cancelTimer(const std::string& id) {
        FeatureError e;
        auto it = std::find_if(timers_.begin(), timers_.end(),
                               [&](const TimerItem& t) { return t.id == id; });
        if (it == timers_.end()) { e.ok = false; e.code = "not_found"; return e; }
        timers_.erase(it);
        return e;
    }

    void cancelAllTimers() { timers_.clear(); }

    // ---- 闹钟 ----
    FeatureError createAlarm(int64_t hour, int64_t minute, int repeat,
                             const std::string& label, int64_t now_wall_ms,
                             std::string* out_id = nullptr) {
        FeatureError e;
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59
            || repeat < 0 || repeat > 2) {
            e.ok = false; e.code = "invalid_arg"; return e;
        }
        if (alarms_.size() >= kMaxAlarms) { e.ok = false; e.code = "limit_exceeded"; return e; }
        AlarmItem a;
        a.id = nextId("a");
        a.label = label;
        a.hour = hour; a.minute = minute; a.repeat = repeat;
        a.next_fire_wall_ms = computeNextFire(a, now_wall_ms);
        alarms_.push_back(a);
        if (out_id) *out_id = a.id;
        return e;
    }

    std::vector<AlarmItem> listAlarms() const { return alarms_; }

    FeatureError enableAlarm(const std::string& id, bool on) {
        FeatureError e;
        auto it = findAlarm(id);
        if (it == alarms_.end()) { e.ok = false; e.code = "not_found"; return e; }
        it->enabled = on;
        return e;
    }

    FeatureError deleteAlarm(const std::string& id) {
        FeatureError e;
        auto it = findAlarm(id);
        if (it == alarms_.end()) { e.ok = false; e.code = "not_found"; return e; }
        alarms_.erase(it);
        return e;
    }

    void deleteAllAlarms() { alarms_.clear(); }

    // ---- 便签 ----
    FeatureError addNote(const std::string& text, int64_t now_wall_ms,
                         std::string* out_id = nullptr) {
        FeatureError e;
        if (text.empty()) { e.ok = false; e.code = "invalid_arg"; return e; }
        if (countUtf8(text) > kMaxNoteChars) { e.ok = false; e.code = "invalid_arg"; return e; }
        if (notes_.size() >= kMaxNotes) { e.ok = false; e.code = "limit_exceeded"; return e; }
        NoteItem n;
        n.id = nextId("n");
        n.text = text;
        n.created_wall_ms = now_wall_ms;
        notes_.push_back(n);
        if (out_id) *out_id = n.id;
        return e;
    }

    std::vector<NoteItem> listNotes(int limit) const {
        std::vector<NoteItem> out = notes_;
        // 新到旧
        std::reverse(out.begin(), out.end());
        if (limit > 0 && static_cast<int>(out.size()) > limit)
            out.resize(static_cast<size_t>(limit));
        return out;
    }

    bool readNote(const std::string& id, std::string* out_text) const {
        auto it = std::find_if(notes_.begin(), notes_.end(),
                               [&](const NoteItem& n) { return n.id == id; });
        if (it == notes_.end()) return false;
        if (out_text) *out_text = it->text;
        return true;
    }

    FeatureError deleteNote(const std::string& id) {
        FeatureError e;
        auto it = std::find_if(notes_.begin(), notes_.end(),
                               [&](const NoteItem& n) { return n.id == id; });
        if (it == notes_.end()) { e.ok = false; e.code = "not_found"; return e; }
        notes_.erase(it);
        return e;
    }

    void deleteAllNotes() { notes_.clear(); }

    // ---- 显示模式 ----
    void setDisplayMode(DisplayMode m) { display_mode_ = m; }
    DisplayMode displayMode() const { return display_mode_; }

    // R8_R4_R3_R4: AI 模式 (auto|fast|deep|local, 菜单选择持久化, router 真实绑定)
    const std::string& aiMode() const { return ai_mode_; }
    // R5_R4 (B2): 停止当前响铃 (不删除定时器/闹钟配置);
    // 主循环用 consumeAlertDismiss 消费并清横幅/停止蜂鸣
    void dismissAlert() { alert_dismissed_ = true; }
    bool consumeAlertDismiss() {
        const bool v = alert_dismissed_;
        alert_dismissed_ = false;
        return v;
    }
    void setAiMode(const std::string& m) {
        ai_mode_ = (m == "fast" || m == "deep" || m == "local") ? m : "auto";
    }

    // ---- 调度 (每帧/周期调用; 单调+墙钟注入) ----
    std::vector<FireEvent> tick(int64_t now_mono_ms, int64_t now_wall_ms) {
        std::vector<FireEvent> out;
        for (auto& t : timers_) {
            if (!t.fired && now_mono_ms >= t.ends_mono_ms) {
                t.fired = true;
                out.push_back({"timer", t.id});
            }
        }
        for (auto& a : alarms_) {
            if (!a.enabled) continue;
            if (a.next_fire_wall_ms > 0 && now_wall_ms >= a.next_fire_wall_ms) {
                out.push_back({"alarm", a.id});
                if (a.repeat == 0) {
                    a.fired_once = true;
                    a.enabled = false;
                } else {
                    a.next_fire_wall_ms = computeNextFire(a, now_wall_ms + 60000);
                }
            }
        }
        return out;
    }

    // ---- 持久化 (原子替换; 损坏保留副本并回空安全状态) ----
    FeatureError save() {
        FeatureError e;
        if (store_path_.empty()) { e.ok = false; e.code = "persist_error"; return e; }
        {
            std::error_code dec;
            std::filesystem::create_directories(store_path_.parent_path(), dec);
        }
        std::string tmp = store_path_.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { e.ok = false; e.code = "persist_error"; return e; }
            f << serialize();
            f.flush();
            if (!f) { e.ok = false; e.code = "persist_error"; return e; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, store_path_, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            e.ok = false; e.code = "persist_error";
        }
        return e;
    }

    FeatureError load(int64_t now_mono_ms = 0, int64_t now_wall_ms = 0) {
        FeatureError e;
        now_mono_at_load_ = now_mono_ms;
        now_wall_at_load_ = now_wall_ms;
        if (store_path_.empty() || !std::filesystem::exists(store_path_)) {
            return e;   // 无文件 = 空状态
        }
        std::string body;
        {
            std::ifstream f(store_path_, std::ios::binary);
            if (!f) { e.ok = false; e.code = "persist_error"; return e; }
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
        }
        if (!deserialize(body)) {
            // 保留损坏副本, 加载空安全状态, 明确告警短码
            std::error_code ec;
            std::filesystem::rename(store_path_,
                                    store_path_.string() + ".corrupt", ec);
            timers_.clear(); alarms_.clear(); notes_.clear();
            display_mode_ = DisplayMode::Pet;
            e.ok = false; e.code = "corrupt_recovered";
        }
        return e;
    }

    // ---- 数据 (测试可注入观察) ----
    static constexpr int kMaxTimers = 8;
    static constexpr int kMaxAlarms = 16;
    static constexpr int kMaxNotes = 100;
    static constexpr int kMaxNoteChars = 500;

private:
    std::vector<AlarmItem>::iterator findAlarm(const std::string& id) {
        return std::find_if(alarms_.begin(), alarms_.end(),
                            [&](const AlarmItem& a) { return a.id == id; });
    }

    std::string nextId(const char* prefix) {
        return std::string(prefix) + std::to_string(++id_seq_);
    }

    static int countUtf8(const std::string& s) {
        int n = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c & 0xC0) != 0x80) ++n;   // 非续字节 = 新码点
        }
        return n;
    }

    // 下一次触发墙钟: 今天/明天 (本地时区语义由调用方注入的 wall 值保证)
    static int64_t computeNextFire(const AlarmItem& a, int64_t now_wall_ms) {
        int64_t day = 24 * 3600 * 1000LL;
        int64_t sod = (a.hour * 3600 + a.minute * 60) * 1000LL;
        int64_t base = now_wall_ms - (now_wall_ms % day);
        int64_t cand = base + sod;
        if (cand <= now_wall_ms) cand += day;
        if (a.repeat == 2) {   // 工作日: 跳过周末 (周六=5 周日=6, 以 epoch 1970-01-01=周四)
            for (int guard = 0; guard < 7; ++guard) {
                int64_t dow = ((cand / day) + 4) % 7;   // 0=周日 ... 6=周六
                if (dow >= 1 && dow <= 5) break;
                cand += day;
            }
        }
        return cand;
    }

    std::string serialize() const {
        std::ostringstream ss;
        ss << "{\"schema_version\":1,\"display\":"
           << (display_mode_ == DisplayMode::Clock ? "\"clock\"" : "\"pet\"")
           << ",\"ai_mode\":\"" << esc(ai_mode_) << "\""
           << ",\"timers\":[";
        for (size_t i = 0; i < timers_.size(); ++i) {
            const auto& t = timers_[i];
            if (i) ss << ",";
            ss << "{\"id\":\"" << t.id << "\",\"label\":\"" << esc(t.label)
               << "\",\"duration_ms\":" << t.duration_ms
               << ",\"ends_mono_ms\":" << t.ends_mono_ms
               << ",\"ends_wall_ms\":" << t.ends_wall_ms
               << ",\"fired\":" << (t.fired ? "true" : "false") << "}";
        }
        ss << "],\"alarms\":[";
        for (size_t i = 0; i < alarms_.size(); ++i) {
            const auto& a = alarms_[i];
            if (i) ss << ",";
            ss << "{\"id\":\"" << a.id << "\",\"label\":\"" << esc(a.label)
               << "\",\"hour\":" << a.hour << ",\"minute\":" << a.minute
               << ",\"repeat\":" << a.repeat
               << ",\"enabled\":" << (a.enabled ? "true" : "false")
               << ",\"fired_once\":" << (a.fired_once ? "true" : "false")
               << ",\"next_fire_wall_ms\":" << a.next_fire_wall_ms << "}";
        }
        ss << "],\"notes\":[";
        for (size_t i = 0; i < notes_.size(); ++i) {
            const auto& n = notes_[i];
            if (i) ss << ",";
            ss << "{\"id\":\"" << n.id << "\",\"text\":\"" << esc(n.text)
               << "\",\"created_wall_ms\":" << n.created_wall_ms << "}";
        }
        ss << "]}";
        return ss.str();
    }

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else o += c;
        }
        return o;
    }

    // 极简反序列化: 逐字段手工解析 (只信任 schema_version==1 且结构完整)
    bool deserialize(const std::string& body) {
        // 依赖 mini_json (src/system/mini_json.hpp) 会更稳; 此处用有界字段扫描
        // 保证不崩溃: 任何不一致 → false。
        auto find = [&](const std::string& key) -> std::string {
            size_t p = body.find("\"" + key + "\"");
            if (p == std::string::npos) return "";
            p = body.find(':', p);
            if (p == std::string::npos) return "";
            ++p;
            while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
            size_t e = p;
            while (e < body.size() && body[e] != ',' && body[e] != '}'
                   && body[e] != ']') ++e;
            return body.substr(p, e - p);
        };
        {
            int depth = 0;
            bool in_str = false, esc = false;
            for (char c : body) {
                if (in_str) {
                    if (esc) esc = false;
                    else if (c == 0x5c) esc = true;
                    else if (c == '"') in_str = false;
                    continue;
                }
                if (c == '"') in_str = true;
                else if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') { if (--depth < 0) return false; }
            }
            if (depth != 0 || in_str) return false;
        }
        std::string sv = find("schema_version");
        if (sv != "1") return false;
        std::string disp = find("display");
        if (disp == "\"clock\"") display_mode_ = DisplayMode::Clock;
        else if (disp == "\"pet\"" || disp.empty()) display_mode_ = DisplayMode::Pet;
        else return false;
        {
            std::string am = find("ai_mode");
            am = (am == "\"fast\"" || am == "\"deep\"" || am == "\"local\"")
                ? am.substr(1, am.size() - 2) : "auto";
            ai_mode_ = am;
        }
        // 定时器恢复: 持久化目标墙钟 → 用当前注入时钟重算单调到期
        size_t p = 0;
        while ((p = body.find("\"id\":\"t", p)) != std::string::npos) {
            size_t id_e = body.find('"', p + 6);
            size_t d_s = body.find("\"duration_ms\":", p);
            size_t e_s = body.find("\"ends_wall_ms\":", p);
            if (id_e == std::string::npos || d_s == std::string::npos
                || e_s == std::string::npos) return false;
            TimerItem it;
            it.id = body.substr(p + 6, id_e - p - 6);
            it.duration_ms = std::atoll(body.substr(d_s + 14).c_str());
            it.ends_wall_ms = std::atoll(body.substr(e_s + 15).c_str());
            it.ends_mono_ms = now_mono_at_load_
                              + (it.ends_wall_ms - now_wall_at_load_);
            timers_.push_back(it);
            p = e_s + 15;
        }
        // 闹钟恢复 (next_fire 已持久化)
        p = 0;
        while ((p = body.find("\"id\":\"a", p)) != std::string::npos) {
            size_t id_e = body.find('"', p + 6);
            size_t h_s = body.find("\"hour\":", p);
            size_t m_s = body.find("\"minute\":", p);
            size_t r_s = body.find("\"repeat\":", p);
            size_t en_s = body.find("\"enabled\":", p);
            size_t fo_s = body.find("\"fired_once\":", p);
            size_t n_s = body.find("\"next_fire_wall_ms\":", p);
            if (id_e == std::string::npos || h_s == std::string::npos
                || m_s == std::string::npos || r_s == std::string::npos
                || en_s == std::string::npos || fo_s == std::string::npos
                || n_s == std::string::npos) return false;
            AlarmItem ia;
            ia.id = body.substr(p + 6, id_e - p - 6);
            ia.hour = std::atoll(body.substr(h_s + 7).c_str());
            ia.minute = std::atoll(body.substr(m_s + 9).c_str());
            ia.repeat = static_cast<int>(std::atoll(body.substr(r_s + 9).c_str()));
            ia.enabled = body.substr(en_s + 10, 4) == "true";
            ia.fired_once = body.substr(fo_s + 13, 4) == "true";
            ia.next_fire_wall_ms = std::atoll(body.substr(n_s + 20).c_str());
            alarms_.push_back(ia);
            p = n_s + 20;
        }
        // 便签完整正文恢复 (列表仅摘要展示; 存储层保留全文, 日志不落)
        p = 0;
        int parsed = 0;
        while ((p = body.find("\"id\":\"n", p)) != std::string::npos) {
            size_t id_e = body.find('"', p + 6);
            size_t t_s = body.find("\"text\":\"", p);
            size_t c_s = body.find("\"created_wall_ms\":", p);
            if (id_e == std::string::npos || t_s == std::string::npos
                || c_s == std::string::npos) return false;
            size_t t_e = t_s + 8;
            while (t_e < body.size()) {
                if (body[t_e] == '\\') t_e += 2;
                else if (body[t_e] == '"') break;
                else ++t_e;
            }
            if (t_e >= body.size()) return false;
            NoteItem n;
            n.id = body.substr(p + 6, id_e - p - 6);
            n.text = unesc(body.substr(t_s + 8, t_e - t_s - 8));
            n.created_wall_ms = std::atoll(body.substr(
                c_s + 18).c_str());
            notes_.push_back(n);
            ++parsed;
            p = t_e;
        }
        (void)parsed;
        return true;
    }

    static std::string unesc(const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[++i];
                if (n == 'n') o += '\n';
                else if (n == 'r') o += '\r';
                else o += n;
            } else o += s[i];
        }
        return o;
    }

    std::filesystem::path store_path_;
    std::vector<TimerItem> timers_;
    std::vector<AlarmItem> alarms_;
    std::vector<NoteItem> notes_;
    DisplayMode display_mode_ = DisplayMode::Pet;
    std::string ai_mode_ = "auto";   // R8_R4_R3_R4
    bool alert_dismissed_ = false;   // R5_R4: 响铃停止信号 (一次性消费)
    int64_t id_seq_ = 0;
    int64_t now_mono_at_load_ = 0;
    int64_t now_wall_at_load_ = 0;
};

} // namespace holopet
