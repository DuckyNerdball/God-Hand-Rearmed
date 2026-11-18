#pragma once

// A small JSON reader (the launcher's GitHub release list, the saloon's layer layout): the whole text into a tree.
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace kamite::json
{
    struct Json
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        } kind = Kind::Null;
        bool flag = false;
        double number = 0.0;
        std::string text;
        std::vector<Json> items;
        std::vector<std::pair<std::string, Json>> members;

        const Json *operator[](const char *key) const
        {
            for (const auto &member : members)
                if (member.first == key)
                    return &member.second;
            return nullptr;
        }
    };

    class JsonReader
    {
    public:
        explicit JsonReader(const std::string &text) : m_text(text) {}
        bool read(Json &out)
        {
            if (!value(out, 0))
                return false;
            space();
            return m_at == m_text.size();
        }

    private:
        void space()
        {
            while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at])))
                ++m_at;
        }
        bool literal(const char *word)
        {
            const size_t n = std::strlen(word);
            if (m_text.compare(m_at, n, word) != 0)
                return false;
            m_at += n;
            return true;
        }
        static void utf8(std::string &out, uint32_t c)
        {
            if (c < 0x80)
                out += static_cast<char>(c);
            else if (c < 0x800)
            {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        bool hex4(uint32_t &out)
        {
            if (m_at + 4 > m_text.size())
                return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = m_text[m_at++];
                out <<= 4;
                if (c >= '0' && c <= '9')
                    out |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    out |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    out |= static_cast<uint32_t>(c - 'A' + 10);
                else
                    return false;
            }
            return true;
        }
        bool string(std::string &out)
        {
            if (m_at >= m_text.size() || m_text[m_at] != '"')
                return false;
            ++m_at;
            while (m_at < m_text.size())
            {
                const char c = m_text[m_at++];
                if (c == '"')
                    return true;
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (m_at >= m_text.size())
                    return false;
                const char e = m_text[m_at++];
                switch (e)
                {
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 'b':
                case 'f':
                    break;
                case 'u':
                {
                    uint32_t c1 = 0;
                    if (!hex4(c1))
                        return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && m_text.compare(m_at, 2, "\\u") == 0)
                    {
                        m_at += 2;
                        uint32_t c2 = 0;
                        if (!hex4(c2))
                            return false;
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    utf8(out, c1);
                    break;
                }
                default:
                    out += e; // \" \\ \/
                    break;
                }
            }
            return false;
        }
        bool value(Json &out, int depth)
        {
            if (depth > 32)
                return false;
            space();
            if (m_at >= m_text.size())
                return false;
            const char c = m_text[m_at];
            if (c == '{')
            {
                out.kind = Json::Kind::Object;
                ++m_at;
                space();
                if (m_at < m_text.size() && m_text[m_at] == '}')
                    return ++m_at, true;
                for (;;)
                {
                    space();
                    std::string key;
                    if (!string(key))
                        return false;
                    space();
                    if (m_at >= m_text.size() || m_text[m_at++] != ':')
                        return false;
                    Json item;
                    if (!value(item, depth + 1))
                        return false;
                    out.members.emplace_back(std::move(key), std::move(item));
                    space();
                    if (m_at >= m_text.size())
                        return false;
                    const char next = m_text[m_at++];
                    if (next == '}')
                        return true;
                    if (next != ',')
                        return false;
                }
            }
            if (c == '[')
            {
                out.kind = Json::Kind::Array;
                ++m_at;
                space();
                if (m_at < m_text.size() && m_text[m_at] == ']')
                    return ++m_at, true;
                for (;;)
                {
                    Json item;
                    if (!value(item, depth + 1))
                        return false;
                    out.items.push_back(std::move(item));
                    space();
                    if (m_at >= m_text.size())
                        return false;
                    const char next = m_text[m_at++];
                    if (next == ']')
                        return true;
                    if (next != ',')
                        return false;
                }
            }
            if (c == '"')
            {
                out.kind = Json::Kind::String;
                return string(out.text);
            }
            if (literal("true") || literal("false"))
            {
                out.kind = Json::Kind::Bool;
                out.flag = m_text[m_at - 1] == 'e' && m_text[m_at - 2] == 'u';
                return true;
            }
            if (literal("null"))
                return true;
            const size_t start = m_at;
            while (m_at < m_text.size() && (std::isdigit(static_cast<unsigned char>(m_text[m_at])) || std::strchr("+-.eE", m_text[m_at])))
                ++m_at;
            if (m_at == start)
                return false;
            out.kind = Json::Kind::Number;
            out.number = std::atof(m_text.substr(start, m_at - start).c_str());
            return true;
        }

        const std::string &m_text;
        size_t m_at = 0;
    };

    inline double number(const Json *value, double fallback = 0.0)
    {
        return value && value->kind == Json::Kind::Number ? value->number : fallback;
    }
}
