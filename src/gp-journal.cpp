/*
GamePulse for OBS — session journal + exporters implementation.
*/

#include "gp-journal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

#include <util/platform.h>
#include <util/base.h>

namespace gamepulse {

namespace {

std::string json_escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char raw : s) {
		unsigned char c = static_cast<unsigned char>(raw);
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

std::string csv_escape(const std::string &s)
{
	bool needs_quotes = s.find_first_of(",\"\n\r") != std::string::npos;
	if (!needs_quotes)
		return s;
	std::string out = "\"";
	for (char c : s) {
		if (c == '"')
			out += "\"\"";
		else
			out += c;
	}
	out += "\"";
	return out;
}

std::string local_time_string(int64_t wall_ms, const char *fmt)
{
	time_t t = static_cast<time_t>(wall_ms / 1000);
	struct tm tm_buf;
#ifdef _WIN32
	localtime_s(&tm_buf, &t);
#else
	localtime_r(&t, &tm_buf);
#endif
	char buf[64];
	strftime(buf, sizeof(buf), fmt, &tm_buf);
	return buf;
}

std::string iso_time(int64_t wall_ms)
{
	std::string base = local_time_string(wall_ms, "%Y-%m-%dT%H:%M:%S");
	char frac[8];
	snprintf(frac, sizeof(frac), ".%03d", static_cast<int>(wall_ms % 1000));
	return base + frac;
}

/* EDL timecode at 30 fps non-drop */
std::string edl_timecode(int64_t ms, int64_t frame_offset = 0)
{
	if (ms < 0)
		ms = 0;
	int64_t frames_total = (ms * 30) / 1000 + frame_offset;
	int64_t f = frames_total % 30;
	int64_t s = (frames_total / 30) % 60;
	int64_t m = (frames_total / (30 * 60)) % 60;
	int64_t h = frames_total / (30 * 3600);
	char buf[32];
	snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s,
		 (long long)f);
	return buf;
}

std::string actions_string(uint32_t a)
{
	std::string out;
	auto add = [&](const char *s) {
		if (!out.empty())
			out += "+";
		out += s;
	};
	if (a & ACTION_CHAPTER)
		add("chapter");
	if (a & ACTION_CLIP)
		add("clip");
	if (a & ACTION_MARKER)
		add("marker");
	if (a & ACTION_OVERLAY)
		add("overlay");
	if (a & ACTION_CAPTION)
		add("caption");
	if (out.empty())
		out = "log";
	return out;
}

const char *source_string(EventSource s)
{
	switch (s) {
	case EventSource::Manual:
		return "manual";
	case EventSource::Chat:
		return "chat";
	default:
		return "gep";
	}
}

} // namespace

void Journal::set_base_dir(const std::string &base_dir)
{
	base_dir_ = base_dir;
}

void Journal::begin_session(const char *reason)
{
	if (session_open_)
		return;

	session_started_wall_ms_ = static_cast<int64_t>(time(nullptr)) * 1000;

	session_id_ = local_time_string(session_started_wall_ms_, "%Y%m%d-%H%M%S");
	session_dir_ = base_dir_ + "/" + session_id_;
	os_mkdirs(session_dir_.c_str());
	entries_.clear();
	session_open_ = true;

	blog(LOG_INFO, "[gamepulse] journal session %s started (%s)", session_id_.c_str(), reason ? reason : "");
}

void Journal::append(const GpEvent &ev)
{
	if (!session_open_)
		begin_session("first event");

	JournalEntry e;
	e.wall_ms = ev.wall_ms;
	e.stream_ms = ev.stream_ms;
	e.record_ms = ev.record_ms;
	e.game_name = ev.game_name;
	e.name = ev.name;
	e.label = ev.label;
	e.detail = ev.detail;
	e.importance = ev.importance;
	e.actions = ev.actions_taken;
	e.source = source_string(ev.source);
	entries_.push_back(std::move(e));

	/* Keep the on-disk JSON current so a crash can't lose the session. Cheap
	   at human event rates. */
	write_json();
}

std::string Journal::json_path() const
{
	return session_dir_ + "/session.json";
}

void Journal::write_json() const
{
	if (!session_open_)
		return;

	std::ostringstream o;
	o << "{\n";
	o << "  \"session_id\": \"" << json_escape(session_id_) << "\",\n";
	o << "  \"started_at\": \"" << iso_time(session_started_wall_ms_) << "\",\n";
	o << "  \"generator\": \"obs-gamepulse\",\n";
	o << "  \"events\": [\n";
	for (size_t i = 0; i < entries_.size(); i++) {
		const JournalEntry &e = entries_[i];
		o << "    {\"wall\": \"" << iso_time(e.wall_ms) << "\"";
		o << ", \"wall_ms\": " << e.wall_ms;
		if (e.stream_ms >= 0)
			o << ", \"stream_ms\": " << e.stream_ms;
		if (e.record_ms >= 0)
			o << ", \"record_ms\": " << e.record_ms;
		if (!e.game_name.empty())
			o << ", \"game\": \"" << json_escape(e.game_name) << "\"";
		o << ", \"event\": \"" << json_escape(e.name) << "\"";
		o << ", \"label\": \"" << json_escape(e.label) << "\"";
		if (!e.detail.empty())
			o << ", \"detail\": \"" << json_escape(e.detail) << "\"";
		o << ", \"importance\": " << e.importance;
		o << ", \"actions\": \"" << actions_string(e.actions) << "\"";
		o << ", \"source\": \"" << e.source << "\"}";
		o << (i + 1 < entries_.size() ? ",\n" : "\n");
	}
	o << "  ]\n}\n";

	os_quick_write_utf8_file(json_path().c_str(), o.str().c_str(), o.str().size(), false);
}

std::string Journal::render_youtube() const
{
	/* YouTube rules: first chapter must be 0:00, chapters >= 10s apart. */
	std::ostringstream o;
	o << "0:00 Stream start\n";
	int64_t last_ts = 0;
	for (const JournalEntry &e : entries_) {
		if (e.stream_ms < 0 || e.importance < IMP_NOTABLE)
			continue;
		int64_t ts = e.stream_ms;
		if (ts - last_ts < 10000 && last_ts != 0)
			continue; /* keep >= 10s spacing */
		if (ts < 10000)
			continue; /* would collide with 0:00 */
		o << format_youtube(ts) << " " << e.label;
		if (!e.detail.empty())
			o << " - " << e.detail;
		if (!e.game_name.empty())
			o << " (" << e.game_name << ")";
		o << "\n";
		last_ts = ts;
	}
	return o.str();
}

std::string Journal::render_csv() const
{
	std::ostringstream o;
	o << "wall_time,stream_time,record_time,game,event,label,detail,importance,actions,source\n";
	for (const JournalEntry &e : entries_) {
		o << iso_time(e.wall_ms) << ",";
		o << (e.stream_ms >= 0 ? format_clock(e.stream_ms) : "") << ",";
		o << (e.record_ms >= 0 ? format_clock(e.record_ms) : "") << ",";
		o << csv_escape(e.game_name) << ",";
		o << csv_escape(e.name) << ",";
		o << csv_escape(e.label) << ",";
		o << csv_escape(e.detail) << ",";
		o << e.importance << ",";
		o << actions_string(e.actions) << ",";
		o << e.source << "\n";
	}
	return o.str();
}

std::string Journal::render_edl() const
{
	/* DaVinci Resolve marker-import EDL. Timebase 30 fps NDF; Resolve maps
	   record timecodes onto the timeline start (usually 01:00:00:00), so we
	   offset by one hour like exported timelines do. */
	const int64_t hour_ms = 3600 * 1000;
	std::ostringstream o;
	o << "TITLE: GamePulse " << session_id_ << "\n";
	o << "FCM: NON-DROP FRAME\n\n";
	int n = 1;
	for (const JournalEntry &e : entries_) {
		int64_t base = e.record_ms >= 0 ? e.record_ms : e.stream_ms;
		if (base < 0 || e.importance < IMP_MINOR)
			continue;
		const char *color = e.importance >= IMP_EPIC      ? "ResolveColorRed"
				    : e.importance == IMP_NOTABLE ? "ResolveColorGreen"
								  : "ResolveColorBlue";
		std::string tc_in = edl_timecode(base + hour_ms);
		std::string tc_out = edl_timecode(base + hour_ms, 1);
		char line[128];
		snprintf(line, sizeof(line), "%03d  001      V     C        %s %s %s %s", n, tc_in.c_str(),
			 tc_out.c_str(), tc_in.c_str(), tc_out.c_str());
		o << line << "\n";
		std::string name = e.label;
		if (!e.detail.empty())
			name += " - " + e.detail;
		o << " |C:" << color << " |M:" << name << " |D:1\n\n";
		n++;
	}
	return o.str();
}

std::string Journal::export_files(unsigned which)
{
	if (!session_open_)
		return "";

	if (which & 1)
		write_json();
	if (which & 2) {
		std::string s = render_youtube();
		os_quick_write_utf8_file((session_dir_ + "/youtube-chapters.txt").c_str(), s.c_str(), s.size(), false);
	}
	if (which & 4) {
		std::string s = render_csv();
		os_quick_write_utf8_file((session_dir_ + "/events.csv").c_str(), s.c_str(), s.size(), false);
	}
	if (which & 8) {
		std::string s = render_edl();
		os_quick_write_utf8_file((session_dir_ + "/markers.edl").c_str(), s.c_str(), s.size(), false);
	}
	blog(LOG_INFO, "[gamepulse] exported session files to %s", session_dir_.c_str());
	return session_dir_;
}

void Journal::end_session(bool auto_export)
{
	if (!session_open_)
		return;
	if (auto_export)
		export_files();
	else
		write_json();
	blog(LOG_INFO, "[gamepulse] journal session %s ended (%zu events)", session_id_.c_str(), entries_.size());
	session_open_ = false;
}

std::string Journal::summary() const
{
	size_t clips = 0, chapters = 0, markers = 0;
	for (const JournalEntry &e : entries_) {
		if (e.actions & ACTION_CLIP)
			clips++;
		if (e.actions & ACTION_CHAPTER)
			chapters++;
		if (e.actions & ACTION_MARKER)
			markers++;
	}
	char buf[160];
	snprintf(buf, sizeof(buf), "%zu events \xC2\xB7 %zu clips \xC2\xB7 %zu chapters \xC2\xB7 %zu markers",
		 entries_.size(), clips, chapters, markers);
	return buf;
}

} // namespace gamepulse
