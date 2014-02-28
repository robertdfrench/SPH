#version 150 core
in vec3 sphere_color;
in vec2 circle_center;

out vec4 out_color;

uniform float radius;

const vec3 light_intensity = vec3(1.0, 1.0, 1.0);
const vec3 light_position = vec3(0.0, 0.0, 0.4);

void main() {
    // Convert from [0,1] to [-1, 1] coords
    vec2 local_frag_coord = (2.0 * gl_PointCoord) - 1.0;

    local_frag_coord.y = -local_frag_coord.y;

    // squared 2D distance from center of gl_point
    float rad_squared = dot(local_frag_coord, local_frag_coord);

    // If outside of the 2D circle discard
    if(rad_squared > 1.0)
        discard;

    // Calculate 3D normal
    vec3 normal = normalize( vec3(local_frag_coord, sqrt(1.0 - rad_squared)));

    // GL world coordinates
    vec3 frag_position = (normal * radius/1024.0) + vec3(circle_center, 0.0);

    // Vector from frag to light
    vec3 frag_to_light = normalize( light_position - frag_position );

    // cosine of angle of incidence
    float cosAngleIncidence = clamp( 1.0 * dot(normal, frag_to_light) , 0, 1);
    cosAngleIncidence = cosAngleIncidence < 0.0001 ? 0.0 : cosAngleIncidence;

    vec3 color = sqrt(sphere_color * light_intensity * cosAngleIncidence);

    out_color = vec4(color, 1);
}

