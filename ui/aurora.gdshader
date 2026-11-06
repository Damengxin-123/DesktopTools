shader_type canvas_item;
render_mode unshaded;

// 与工作台同步切换金色和蓝紫色页头。
uniform bool gold_theme = false;

// 低亮度光晕和网格只作为面板背景，留出文本对比度。
void fragment() {
	vec2 p = UV;
	vec2 pixel = 1.0 / vec2(max(fwidth(p.x), 0.0001), max(fwidth(p.y), 0.0001));
	vec2 edge = abs((p - 0.5) * pixel) - pixel * 0.5 + vec2(13.0);
	float distance_to_edge = length(max(edge, 0.0)) + min(max(edge.x, edge.y), 0.0) - 13.0;
	vec3 base = vec3(0.075, 0.101, 0.169);
	float violet = exp(-7.0 * length((p - vec2(0.83, -0.08)) * vec2(0.9, 0.56)));
	float cyan = exp(-7.0 * length((p - vec2(1.08, 0.52)) * vec2(1.2, 0.58)));
	vec2 grid = abs(fract(p * vec2(33.0, 8.0)) - 0.5);
	float lines = smoothstep(0.473, 0.5, max(grid.x, grid.y));
	vec3 color = base + violet * vec3(0.28, 0.19, 0.49) + cyan * vec3(0.02, 0.22, 0.27);
	color += lines * 0.047 * smoothstep(0.16, 0.87, p.x);
	vec3 border = vec3(0.20, 0.25, 0.38);
	if (gold_theme) {
		color = mix(vec3(1.0, 0.975, 0.914), vec3(0.95, 0.82, 0.54), violet * 0.85 + cyan * 0.25);
		color -= lines * vec3(0.065, 0.075, 0.085) * smoothstep(0.16, 0.87, p.x);
		border = vec3(0.79, 0.65, 0.40);
	}
	color = mix(color, border, smoothstep(-2.0, -0.4, distance_to_edge) * 0.85);
	COLOR = vec4(color, 1.0 - smoothstep(-0.5, 0.5, distance_to_edge));
}
