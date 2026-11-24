// Convertible core - vector family: the public toolbox for other families.
//
// One include for everything another family may need from the vector
// family. Typical uses:
//
//   PDF family, wiring `svg -> pdf` as true vector output:
//     CVT_TRY_ASSIGN(vector::Scene scene, vector::read_scene("svg", bytes, ctx));
//     CVT_TRY_ASSIGN(auto page, vector::pdf::page_content(scene, 0, ctx));
//     // page.content: the content stream operators; page.objects / resources:
//     // shadings, image XObjects (Flate/DCT, SMask), ExtGStates, patterns and
//     // forms, with {obj N} / {resources} placeholders filled by pdf::resolve().
//
//   Document / office families rasterising an embedded drawing:
//     CVT_TRY_ASSIGN(vector::Scene scene, vector::read_scene("emf", bytes, ctx));
//     CVT_TRY_ASSIGN(ir::Frame frame, vector::render_page(scene, {.dpi = 150}, ctx));
//
//   Any vector format written from a scene:
//     CVT_TRY_ASSIGN(Bytes eps, vector::write_scene("eps", scene, ctx));
//
// Readers registered for read_scene: svg, svgz, eps, ps, ai (PostScript
// based), wmf, emf, wmz, emz, dxf, hpgl, cgm, vectordrawable, lottie, pict.
// Writers for write_scene: svg, svgz, eps, ps, wmf, emf, wmz, emz, dxf, hpgl,
// vectordrawable, tikz.
//
// The scene model (coordinates in CSS px, y down) is in scene.hpp.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include "families/vector/font.hpp"         // TrueType outlines, layout_text, bundled fallback fonts
#include "families/vector/geom.hpp"         // path construction, bounds, flattening, stroking
#include "families/vector/pdf_content.hpp"  // scene page -> PDF content stream + resources
#include "families/vector/render.hpp"       // render_page / render_group (anti-aliased rasteriser)
#include "families/vector/scene.hpp"        // the scene model
#include "families/vector/scene_json.hpp"   // lossless JSON envelope of a scene
#include "families/vector/svg.hpp"          // read_svg / write_svg with explicit options
#include "families/vector/trace.hpp"        // image_to_scene: embed or trace raster pictures
#include "families/vector/vector_codecs.hpp"  // read_scene / write_scene through the registry
