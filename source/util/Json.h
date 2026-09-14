#pragma once

#include <jansson.h>

#include <string>
#include <vector>

// Reading wrapper over jansson: every accessor is total, so a field the
// server stopped sending yields a default instead of a crash.
namespace util {

class Json {
public:
    Json() = default;
    explicit Json(json_t* node, bool owning = false) : m_node(node), m_owning(owning) {}

    Json(const Json& o) : m_node(o.m_node), m_owning(o.m_owning)
    {
        if (m_owning && m_node) json_incref(m_node);
    }

    Json& operator=(const Json& o)
    {
        if (this == &o) return *this;
        reset();
        m_node = o.m_node;
        m_owning = o.m_owning;
        if (m_owning && m_node) json_incref(m_node);
        return *this;
    }

    Json(Json&& o) noexcept : m_node(o.m_node), m_owning(o.m_owning)
    {
        o.m_node = nullptr;
        o.m_owning = false;
    }

    Json& operator=(Json&& o) noexcept
    {
        if (this == &o) return *this;
        reset();
        m_node = o.m_node;
        m_owning = o.m_owning;
        o.m_node = nullptr;
        o.m_owning = false;
        return *this;
    }

    ~Json() { reset(); }

    static Json parse(const std::string& text)
    {
        json_error_t err;
        json_t* root = json_loadb(text.data(), text.size(), 0, &err);
        return Json(root, true);
    }

    bool valid() const { return m_node != nullptr; }
    explicit operator bool() const { return valid(); }

    Json operator[](const char* key) const
    {
        if (!json_is_object(m_node)) return {};
        return Json(json_object_get(m_node, key));
    }

    Json at(size_t i) const
    {
        if (!json_is_array(m_node)) return {};
        return Json(json_array_get(m_node, i));
    }

    size_t size() const { return json_is_array(m_node) ? json_array_size(m_node) : 0; }
    bool isArray() const { return json_is_array(m_node); }
    bool isObject() const { return json_is_object(m_node); }
    bool isNull() const { return !m_node || json_is_null(m_node); }

    std::string str(const char* fallback = "") const
    {
        const char* s = json_is_string(m_node) ? json_string_value(m_node) : nullptr;
        return s ? std::string(s) : std::string(fallback);
    }

    // Distinguishes an absent/null string from an empty one, which matters
    // for artwork URLs: null means "no art", "" would resolve to the origin.
    bool hasStr() const { return json_is_string(m_node) && json_string_length(m_node) > 0; }

    long long num(long long fallback = 0) const
    {
        if (json_is_integer(m_node)) return json_integer_value(m_node);
        if (json_is_real(m_node)) return static_cast<long long>(json_real_value(m_node));
        return fallback;
    }

    double real(double fallback = 0.0) const
    {
        if (json_is_real(m_node)) return json_real_value(m_node);
        if (json_is_integer(m_node)) return static_cast<double>(json_integer_value(m_node));
        return fallback;
    }

    bool boolean(bool fallback = false) const
    {
        if (json_is_boolean(m_node)) return json_boolean_value(m_node);
        return fallback;
    }

    std::vector<std::string> strArray() const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < size(); i++) out.push_back(at(i).str());
        return out;
    }

    json_t* raw() const { return m_node; }

private:
    void reset()
    {
        if (m_owning && m_node) json_decref(m_node);
        m_node = nullptr;
        m_owning = false;
    }

    json_t* m_node = nullptr;
    bool m_owning = false;
};

} // namespace util
