import json
from lottie import objects, Point, Color
from lottie.exporters.core import export_lottie

# 1. Static shapes: rectangle, ellipse, star, polygon, stroke, path.
an = objects.Animation(30, 30)
an.width = 120
an.height = 80
layer = objects.ShapeLayer()
layer.name = "shapes"
an.add_layer(layer)

g = layer.add_shape(objects.Group())
r = g.add_shape(objects.Rect())
r.position.value = Point(20, 20)
r.size.value = Point(30, 20)
g.add_shape(objects.Fill(Color(1, 0, 0)))

g2 = layer.add_shape(objects.Group())
e = g2.add_shape(objects.Ellipse())
e.position.value = Point(60, 20)
e.size.value = Point(20, 20)
g2.add_shape(objects.Fill(Color(0, 0, 1)))
st = g2.add_shape(objects.Stroke(Color(0, 0, 0), 2))

g3 = layer.add_shape(objects.Group())
s = g3.add_shape(objects.Star())
s.position.value = Point(95, 20)
s.outer_radius.value = 15
s.inner_radius.value = 7
s.points.value = 5
s.star_type = objects.StarType.Star
g3.add_shape(objects.Fill(Color(1, 1, 0)))

g4 = layer.add_shape(objects.Group())
p = g4.add_shape(objects.Path())
p.shape.value.add_point(Point(10, 50))
p.shape.value.add_point(Point(50, 50), Point(0, 0), Point(10, 20))
p.shape.value.add_point(Point(50, 75), Point(0, 0), Point(0, 0))
p.shape.value.close()
g4.add_shape(objects.Fill(Color(0, 0.5, 0)))

g5 = layer.add_shape(objects.Group())
poly = g5.add_shape(objects.Star())
poly.position.value = Point(90, 60)
poly.outer_radius.value = 12
poly.points.value = 6
poly.star_type = objects.StarType.Polygon
g5.add_shape(objects.Fill(Color(0.5, 0, 0.5)))
g5.transform.opacity.value = 50

export_lottie(an, "shapes.json")

# 2. Animated: moving, rotating, fading square; keyframes with easing.
an = objects.Animation(60, 30)
an.width = 100
an.height = 100
layer = objects.ShapeLayer()
an.add_layer(layer)
g = layer.add_shape(objects.Group())
r = g.add_shape(objects.Rect())
r.size.value = Point(20, 20)
r.position.value = Point(0, 0)
g.add_shape(objects.Fill(Color(1, 0, 0)))
layer.transform.position.add_keyframe(0, Point(20, 50))
layer.transform.position.add_keyframe(60, Point(80, 50))
layer.transform.rotation.add_keyframe(0, 0)
layer.transform.rotation.add_keyframe(60, 90)
layer.transform.opacity.add_keyframe(0, 100)
layer.transform.opacity.add_keyframe(30, 50)
layer.transform.opacity.add_keyframe(60, 100)
export_lottie(an, "animated.json")

# 3. Gradients, dashes and a precomposition.
an = objects.Animation(30, 30)
an.width = 100
an.height = 60
pre = objects.Precomp()
pre.id = "comp_0"
pl = objects.ShapeLayer()
pg = pl.add_shape(objects.Group())
pr = pg.add_shape(objects.Rect())
pr.position.value = Point(10, 10)
pr.size.value = Point(20, 20)
pg.add_shape(objects.Fill(Color(0, 1, 0)))
pre.add_layer(pl)
an.assets.append(pre)
ref = objects.PreCompLayer("comp_0")
ref.width = 20
ref.height = 20
ref.transform.position.value = Point(70, 30)
ref.transform.anchor_point.value = Point(10, 10)
ref.out_point = 30
an.add_layer(ref)

layer = objects.ShapeLayer()
an.add_layer(layer)
g = layer.add_shape(objects.Group())
r = g.add_shape(objects.Rect())
r.position.value = Point(25, 15)
r.size.value = Point(40, 20)
gf = g.add_shape(objects.GradientFill())
gf.start_point.value = Point(5, 15)
gf.end_point.value = Point(45, 15)
gf.colors.set_stops([(0, Color(1, 0, 0)), (1, Color(0, 0, 1))])
g2 = layer.add_shape(objects.Group())
line = g2.add_shape(objects.Path())
line.shape.value.add_point(Point(5, 45))
line.shape.value.add_point(Point(55, 45))
dst = g2.add_shape(objects.Stroke(Color(0, 0, 0), 3))
dst.dashes = [objects.StrokeDash(6, objects.StrokeDashType.Dash), objects.StrokeDash(4, objects.StrokeDashType.Gap)]
export_lottie(an, "gradient_precomp.json")
