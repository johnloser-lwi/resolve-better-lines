#include "Preset.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace bl {

// =============================================================================
// Writing
// =============================================================================

std::string JsonQuote(const std::string& s)
{
    std::string out = "\"";
    for (char ch : s)
    {
        switch (ch)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters would produce a file that is valid to us
                // and rejected by every other JSON reader, which matters because
                // these files are meant to be editable by hand.
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch & 0xff);
                    out += buf;
                }
                else out += ch;
        }
    }
    return out + "\"";
}

namespace {

/** Numbers are written with enough precision to round-trip a float exactly.
 *  Nine significant digits is the documented minimum for that; the default six
 *  would quietly shift a point every time a preset was saved and reloaded. */
std::string Num(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string ValueToJson(const PresetValue& v)
{
    switch (v.kind)
    {
        case kPKBool:   return v.b ? "true" : "false";
        case kPKString: return JsonQuote(v.s);

        case kPKDouble2D:
        case kPKRGBA:
        {
            std::string out = "[";
            for (int i = 0; i < v.count; ++i)
            {
                if (i) out += ", ";
                out += Num(v.n[i]);
            }
            return out + "]";
        }

        default: return Num(v.n[0]);
    }
}

} // namespace

std::string PresetToJson(const PresetFile& p)
{
    std::ostringstream o;
    o << "{\n";
    o << "  \"plugin\": \"BetterLine\",\n";
    o << "  \"schemaVersion\": " << p.schemaVersion << ",\n";
    o << "  \"name\": " << JsonQuote(p.name) << ",\n";
    o << "  \"params\": {\n";

    // std::map iterates sorted, so a preset saved twice from the same state
    // produces byte-identical files -- which is what makes them diffable and
    // sensible to keep in version control.
    bool first = true;
    for (const auto& kv : p.params)
    {
        if (!first) o << ",\n";
        first = false;
        o << "    " << JsonQuote(kv.first) << ": " << ValueToJson(kv.second);
    }

    o << "\n  }\n}\n";
    return o.str();
}

// =============================================================================
// Reading
// =============================================================================

namespace {

struct Cursor
{
    const std::string& t;
    size_t i = 0;

    explicit Cursor(const std::string& s) : t(s) {}

    void skip()
    {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    }
    bool eof() const { return i >= t.size(); }
    char peek() const { return i < t.size() ? t[i] : '\0'; }
    bool take(char c) { skip(); if (peek() == c) { ++i; return true; } return false; }
};

bool ParseString(Cursor& c, std::string& out, std::string& err)
{
    c.skip();
    if (c.peek() != '"') { err = "expected a string"; return false; }
    ++c.i;

    out.clear();
    while (!c.eof())
    {
        const char ch = c.t[c.i++];
        if (ch == '"') return true;

        if (ch != '\\') { out += ch; continue; }
        if (c.eof()) break;

        const char e = c.t[c.i++];
        switch (e)
        {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                // Only the escapes this writer emits need decoding, and it emits
                // \u exclusively for control characters -- so the low byte is
                // the whole story and no UTF-16 handling is required.
                if (c.i + 4 > c.t.size()) { err = "truncated \\u escape"; return false; }
                const std::string hex = c.t.substr(c.i, 4);
                c.i += 4;
                out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16) & 0xff);
                break;
            }
            // Covers the quote and backslash escapes: the character after the
            // backslash is itself the result.
            //
            // (Deliberately not spelling those two escapes out at the end of
            // this line. A comment ending in a backslash is a line
            // continuation -- it splices the NEXT line into the comment, and
            // the closing brace below disappears with it.)
            default: out += e; break;
        }
    }

    err = "unterminated string";
    return false;
}

bool ParseNumber(Cursor& c, double& out, std::string& err)
{
    c.skip();
    const char* start = c.t.c_str() + c.i;
    char* end = nullptr;
    out = std::strtod(start, &end);
    if (end == start) { err = "expected a number"; return false; }
    c.i += static_cast<size_t>(end - start);
    return true;
}

bool ParseValue(Cursor& c, PresetValue& out, std::string& err)
{
    c.skip();
    const char ch = c.peek();

    if (ch == '"')
    {
        out.kind = kPKString;
        return ParseString(c, out.s, err);
    }
    if (ch == 't' || ch == 'f')
    {
        const bool isTrue = (ch == 't');
        const std::string word = isTrue ? "true" : "false";
        if (c.t.compare(c.i, word.size(), word) != 0) { err = "bad literal"; return false; }
        c.i += word.size();
        out.kind = kPKBool;
        out.b    = isTrue;
        return true;
    }
    if (ch == '[')
    {
        ++c.i;
        out.kind  = kPKDouble2D;   // refined by the caller against the registry
        out.count = 0;
        c.skip();
        if (c.peek() == ']') { ++c.i; return true; }

        while (true)
        {
            double v = 0.0;
            if (!ParseNumber(c, v, err)) return false;
            if (out.count < 4) out.n[out.count++] = v;
            c.skip();
            if (c.take(',')) continue;
            if (c.take(']')) return true;
            err = "expected , or ] in array";
            return false;
        }
    }

    out.kind  = kPKDouble;
    out.count = 1;
    return ParseNumber(c, out.n[0], err);
}

/** Skip a value without recording it, so unknown keys do not stop the parse. */
bool SkipValue(Cursor& c, std::string& err)
{
    c.skip();
    if (c.peek() == '{')
    {
        int depth = 0;
        while (!c.eof())
        {
            const char ch = c.t[c.i];
            if (ch == '"') { std::string junk; if (!ParseString(c, junk, err)) return false; continue; }
            ++c.i;
            if (ch == '{') ++depth;
            else if (ch == '}') { if (--depth == 0) return true; }
        }
        err = "unterminated object";
        return false;
    }
    PresetValue junk;
    return ParseValue(c, junk, err);
}

} // namespace

bool ParseJsonObject(const std::string& json, PresetMap& out, std::string& error)
{
    Cursor c(json);
    if (!c.take('{')) { error = "expected {"; return false; }

    PresetMap parsed;

    c.skip();
    if (c.take('}')) { out.swap(parsed); return true; }

    while (true)
    {
        std::string key;
        if (!ParseString(c, key, error)) return false;
        if (!c.take(':')) { error = "expected : after key \"" + key + "\""; return false; }

        PresetValue v;
        if (!ParseValue(c, v, error)) return false;
        parsed[key] = v;

        if (c.take(',')) continue;
        if (c.take('}')) break;
        error = "expected , or } after \"" + key + "\"";
        return false;
    }

    out.swap(parsed);
    return true;
}

bool PresetFromJson(const std::string& json, PresetFile& out, std::string& error)
{
    Cursor c(json);
    if (!c.take('{')) { error = "expected {"; return false; }

    PresetFile parsed;
    bool sawParams = false;

    c.skip();
    if (c.take('}')) { error = "preset contains no parameters"; return false; }

    while (true)
    {
        std::string key;
        if (!ParseString(c, key, error)) return false;
        if (!c.take(':')) { error = "expected : after key \"" + key + "\""; return false; }

        if (key == "params")
        {
            // Captured as raw text and handed to the shared object parser, so
            // nested-object handling exists in one place only.
            c.skip();
            const size_t start = c.i;
            if (!SkipValue(c, error)) return false;
            const std::string body = c.t.substr(start, c.i - start);
            if (!ParseJsonObject(body, parsed.params, error)) return false;
            sawParams = true;
        }
        else if (key == "name")
        {
            if (!ParseString(c, parsed.name, error)) return false;
        }
        else if (key == "schemaVersion")
        {
            double v = 0.0;
            if (!ParseNumber(c, v, error)) return false;
            parsed.schemaVersion = static_cast<int>(v);
        }
        else
        {
            // Unknown keys are skipped rather than rejected, so a file written
            // by a newer build still loads what this one understands.
            if (!SkipValue(c, error)) return false;
        }

        if (c.take(',')) continue;
        if (c.take('}')) break;
        error = "expected , or } after \"" + key + "\"";
        return false;
    }

    if (!sawParams) { error = "preset has no \"params\" section"; return false; }

    out = parsed;
    return true;
}

std::string SanitiseFileName(const std::string& raw)
{
    std::string out;
    for (char ch : raw)
    {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20) continue;
        if (std::strchr("\\/:*?\"<>|", ch)) continue;
        out += ch;
    }

    // Trailing dots and spaces are legal to write and impossible to open again
    // on Windows, which is a worse failure than refusing the name.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());

    return out;
}

} // namespace bl
