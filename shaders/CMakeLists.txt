# Shader build list. One line per compiled shader: moonsr_add_shader(<file> <profile> <variable>).
# The generated header is <variable>.h and defines `const BYTE <variable>[]`.
#
# Ownership: common/ and this file's structure belong to the lead. Agents append lines in their own section only.

# --- lead: common ---
moonsr_add_shader(common/fullscreen_vs.hlsl vs_5_0 g_vs_fullscreen)
moonsr_add_shader(scale/bilinear_ps.hlsl    ps_5_0 g_ps_bilinear)
moonsr_add_shader(scale/nearest_ps.hlsl     ps_5_0 g_ps_nearest)

# --- shader-engineer: conventional scalers and filters ---
moonsr_add_shader(scale/bicubic_ps.hlsl     ps_5_0 g_ps_bicubic)
moonsr_add_shader(scale/lanczos_ps.hlsl     ps_5_0 g_ps_lanczos)
moonsr_add_shader(scale/sharpen_ps.hlsl     ps_5_0 g_ps_sharpen)

# --- upscaling-engineer: MoonSR custom upscaler ---
moonsr_add_shader(upscale/moonsr_spatial_ps.hlsl ps_5_0 g_ps_moonsr_spatial)
moonsr_add_shader(upscale/alsr_prep_ps.hlsl        ps_5_0 g_ps_alsr_prep)
moonsr_add_shader(upscale/alsr_cellrefine_cs.hlsl  cs_5_0 g_cs_alsr_cellrefine)
moonsr_add_shader(upscale/alsr_motion_ps.hlsl      ps_5_0 g_ps_alsr_motion)
moonsr_add_shader(upscale/alsr_reproject_ps.hlsl   ps_5_0 g_ps_alsr_reproject)
moonsr_add_shader(upscale/alsr_residual_ps.hlsl    ps_5_0 g_ps_alsr_residual)
moonsr_add_shader(upscale/alsr_update_ps.hlsl      ps_5_0 g_ps_alsr_update)
moonsr_add_shader(upscale/alsr_update_ps.hlsl      ps_5_0 g_ps_alsr_update_direct DEFINES ALSR_UPDATE_DIRECT=1)
moonsr_add_shader(upscale/alsr_final_ps.hlsl       ps_5_0 g_ps_alsr_final)
moonsr_add_shader(upscale/alsr_refluma_ps.hlsl     ps_5_0 g_ps_alsr_refluma)
moonsr_add_shader(upscale/alsr_refluma_ps.hlsl     ps_5_0 g_ps_alsr_refy DEFINES ALSR_REF_Y=1)
moonsr_add_shader(upscale/alsr_seed_ps.hlsl        ps_5_0 g_ps_alsr_seed)
moonsr_add_shader(upscale/alsr_seedblend_ps.hlsl   ps_5_0 g_ps_alsr_seedblend)

# --- upscaling-engineer: Smart parallax (screen-space relief, src/relief) ---
moonsr_add_shader(relief/relief_luma_cs.hlsl        cs_5_0 g_cs_relief_luma)
moonsr_add_shader(relief/relief_box_cs.hlsl         cs_5_0 g_cs_relief_box1 DEFINES RELIEF_BOX_PASS=1)
moonsr_add_shader(relief/relief_box_cs.hlsl         cs_5_0 g_cs_relief_box2 DEFINES RELIEF_BOX_PASS=2)
moonsr_add_shader(relief/relief_gate_cs.hlsl        cs_5_0 g_cs_relief_gate)
moonsr_add_shader(relief/relief_height_cs.hlsl      cs_5_0 g_cs_relief_height)
moonsr_add_shader(relief/relief_apply_ps.hlsl       ps_5_0 g_ps_relief_apply)

# --- gpu-rendering-engineer: cursor overlay ---
moonsr_add_shader(cursor/cursor_vs.hlsl     vs_5_0 g_vs_cursor)
moonsr_add_shader(cursor/cursor_ps.hlsl     ps_5_0 g_ps_cursor)

# --- overlay (frame counter) ---
moonsr_add_shader(overlay/overlay_vs.hlsl     vs_5_0 g_vs_overlay)
moonsr_add_shader(overlay/overlay_ps.hlsl     ps_5_0 g_ps_overlay)

# --- frame-generation-engineer ---
moonsr_add_shader(framegen/fg_luma_cs.hlsl         cs_5_0 g_cs_fg_luma)
moonsr_add_shader(framegen/fg_downsample_cs.hlsl   cs_5_0 g_cs_fg_downsample)
moonsr_add_shader(framegen/fg_histogram_cs.hlsl    cs_5_0 g_cs_fg_histogram)
moonsr_add_shader(framegen/fg_search_cs.hlsl       cs_5_0 g_cs_fg_search)
moonsr_add_shader(framegen/fg_search_cs.hlsl       cs_5_0 g_cs_fg_search_r4 DEFINES MAX_R=4)
moonsr_add_shader(framegen/fg_search_cs.hlsl       cs_5_0 g_cs_fg_search_fast DEFINES SEARCH_CENSUS=0)
moonsr_add_shader(framegen/fg_search_cs.hlsl       cs_5_0 g_cs_fg_search_r4_fast DEFINES MAX_R=4 SEARCH_CENSUS=0)
moonsr_add_shader(framegen/fg_regularize_cs.hlsl   cs_5_0 g_cs_fg_regularize)
moonsr_add_shader(framegen/fg_refine_cs.hlsl       cs_5_0 g_cs_fg_refine)
moonsr_add_shader(framegen/fg_cross_cs.hlsl        cs_5_0 g_cs_fg_cross_splat ENTRY splat)
moonsr_add_shader(framegen/fg_cross_cs.hlsl        cs_5_0 g_cs_fg_cross)
moonsr_add_shader(framegen/fg_consistency_cs.hlsl  cs_5_0 g_cs_fg_consistency)
moonsr_add_shader(framegen/fg_motionfrac_cs.hlsl   cs_5_0 g_cs_fg_motionfrac)
moonsr_add_shader(framegen/fg_hudmask_cs.hlsl      cs_5_0 g_cs_fg_hudmask)
moonsr_add_shader(framegen/fg_scenecut_cs.hlsl     cs_5_0 g_cs_fg_scenecut)
moonsr_add_shader(framegen/fg_splat_cs.hlsl        cs_5_0 g_cs_fg_splat)
moonsr_add_shader(framegen/fg_resolve_cs.hlsl      cs_5_0 g_cs_fg_resolve)
moonsr_add_shader(framegen/fg_interpolate_ps.hlsl  ps_5_0 g_ps_fg_interpolate)
moonsr_add_shader(framegen/fg_pixsel_cs.hlsl       cs_5_0 g_cs_fg_pixsel)
moonsr_add_shader(framegen/fg_aa_ps.hlsl           ps_5_0 g_ps_fg_aa)

moonsr_finalize_shaders()
