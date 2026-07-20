//
// Lets the archive save and load GLM types. Vectors become JSON arrays and quats
// save as [x, y, z, w]. These sit in namespace glm so nlohmann can find them.
//

#ifndef YELLOWTAIL_GLMJSON_H
#define YELLOWTAIL_GLMJSON_H

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace glm {
    inline void to_json(nlohmann::json& j, const vec2& v) { j = {v.x, v.y}; }
    inline void from_json(const nlohmann::json& j, vec2& v) { j.at(0).get_to(v.x); j.at(1).get_to(v.y); }

    inline void to_json(nlohmann::json& j, const vec3& v) { j = {v.x, v.y, v.z}; }
    inline void from_json(const nlohmann::json& j, vec3& v) {
        j.at(0).get_to(v.x); j.at(1).get_to(v.y); j.at(2).get_to(v.z);
    }

    inline void to_json(nlohmann::json& j, const vec4& v) { j = {v.x, v.y, v.z, v.w}; }
    inline void from_json(const nlohmann::json& j, vec4& v) {
        j.at(0).get_to(v.x); j.at(1).get_to(v.y); j.at(2).get_to(v.z); j.at(3).get_to(v.w);
    }

    // Saved as [x, y, z, w]. glm::quat's constructor takes (w, x, y, z), so read each part by hand.
    inline void to_json(nlohmann::json& j, const quat& q) { j = {q.x, q.y, q.z, q.w}; }
    inline void from_json(const nlohmann::json& j, quat& q) {
        j.at(0).get_to(q.x); j.at(1).get_to(q.y); j.at(2).get_to(q.z); j.at(3).get_to(q.w);
    }
} // glm

#endif //YELLOWTAIL_GLMJSON_H
