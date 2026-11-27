// What the globe draws: the Earth with its land outlines, the satellites as one points object,
// laser links as line segments, routes as thick lines, ground sites as markers.

import {
  BufferAttribute,
  BufferGeometry,
  Color,
  DynamicDrawUsage,
  Float32BufferAttribute,
  Group,
  InterleavedBuffer,
  LineBasicMaterial,
  LineSegments,
  Mesh,
  MeshBasicMaterial,
  Points,
  ShaderMaterial,
  SphereGeometry,
  type InterleavedBufferAttribute,
} from 'three';
import { LineMaterial } from 'three/examples/jsm/lines/LineMaterial.js';
import { LineSegments2 } from 'three/examples/jsm/lines/LineSegments2.js';
import { LineSegmentsGeometry } from 'three/examples/jsm/lines/LineSegmentsGeometry.js';
import { mesh } from 'topojson-client';
import type { GeometryCollection, Topology } from 'topojson-specification';
import land110m from 'world-atlas/land-110m.json';
import { SHAPE_CODE, type DotShape, type LineStyle } from '../theme';
import { latLonToScene } from './coords';

/** A time that is never reached, for "never marked" (GLSL has no portable infinity). */
export const NEVER = 1e30;

export interface Palette {
  earth: string;
  land: string;
}

export function earth(palette: Palette): Group {
  const group = new Group();
  group.add(new Mesh(new SphereGeometry(1, 96, 64), new MeshBasicMaterial({ color: palette.earth })));
  const topology = land110m as unknown as Topology<{ land: GeometryCollection }>;
  const outline = mesh(topology, topology.objects.land);
  const vertices: number[] = [];
  for (const line of outline.coordinates) {
    for (let i = 1; i < line.length; i++) {
      const a = line[i - 1] as number[];
      const b = line[i] as number[];
      vertices.push(...latLonToScene(a[1] as number, a[0] as number, 1.001));
      vertices.push(...latLonToScene(b[1] as number, b[0] as number, 1.001));
    }
  }
  const geometry = new BufferGeometry();
  geometry.setAttribute('position', new Float32BufferAttribute(vertices, 3));
  group.add(new LineSegments(geometry, new LineBasicMaterial({ color: palette.land })));
  return group;
}

const DOT_VERTEX = /* glsl */ `
  attribute vec3 aColor;
  attribute float aSize;
  attribute float aShape;
  attribute float aMark;
  attribute float aDown;
  uniform float uTime;
  uniform float uScale;
  uniform vec3 uMarkColor;
  uniform float uMarkGrow;
  uniform float uMarkShape;
  uniform vec3 uDownColor;
  uniform float uDownGrow;
  uniform float uDownShape;
  varying vec3 vColor;
  varying float vShape;
  void main() {
    vec4 mv = modelViewMatrix * vec4(position, 1.0);
    gl_Position = projectionMatrix * mv;
    float size = aSize;
    vColor = aColor;
    vShape = aShape;
    if (uTime >= aDown) {
      vColor = uDownColor;
      size *= uDownGrow;
      if (uDownShape >= 0.0) vShape = uDownShape;
    } else if (uTime >= aMark) {
      vColor = uMarkColor;
      size *= uMarkGrow;
      if (uMarkShape >= 0.0) vShape = uMarkShape;
    }
    gl_PointSize = size * uScale;
  }
`;

// The shapes of SHAPE_CODE (theme.ts), the same as ui/marks.ts draws them; p runs from -0.5 to
// 0.5 across the point, y downwards.
const DOT_FRAGMENT = /* glsl */ `
  varying vec3 vColor;
  varying float vShape;
  float segment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
  }
  void main() {
    vec2 p = gl_PointCoord - 0.5;
    float r = length(p);
    vec2 q = abs(p);
    int shape = int(vShape + 0.5);
    bool inside;
    if (shape == 1) inside = r > 0.29 && r < 0.5;
    else if (shape == 2) inside = max(q.x, q.y) < 0.41;
    else if (shape == 3) inside = max(q.x, q.y) < 0.46 && max(q.x, q.y) > 0.31;
    else if (shape == 4) inside = min(segment(p, vec2(-0.36), vec2(0.36)), segment(p, vec2(-0.36, 0.36), vec2(0.36, -0.36))) < 0.12;
    else if (shape == 5) inside = (q.x < 0.14 && q.y < 0.5) || (q.y < 0.14 && q.x < 0.5);
    else if (shape == 6) inside = q.x + q.y < 0.5;
    else if (shape == 7) inside = p.y < 0.35 && q.x < (p.y + 0.5) * 0.54;
    else if (shape == 8) inside = r < 0.17 || (r > 0.33 && r < 0.5);
    else inside = r < 0.5;
    if (!inside) discard;
    gl_FragColor = vec4(vColor, 1.0);
    // Back from three's linear working colours to the page's sRGB, as its own materials do, so
    // a dot's grey is the grey theme.ts names.
    #include <colorspace_fragment>
  }
`;

export interface DotStyle {
  markColor: string;
  markGrow: number;
  /** The shape from aMark on; absent keeps each dot's own shape. */
  markShape?: DotShape;
  downColor: string;
  /** How much a failed dot grows (1.8 when absent). */
  downGrow?: number;
  /** The shape from aDown on; absent keeps each dot's own shape. */
  downShape?: DotShape;
}

/**
 * Dots whose colour, size and shape switch at a time: to the mark style from aMark on (a
 * satellite that learned the news, a detector), to the down style from aDown on (a failed
 * element). Times are story seconds; the shader compares them with uTime and computes nothing
 * else. Each dot has its own shape (aShape, a SHAPE_CODE number) until then.
 */
export function dotMaterial(style: DotStyle): ShaderMaterial {
  return new ShaderMaterial({
    vertexShader: DOT_VERTEX,
    fragmentShader: DOT_FRAGMENT,
    uniforms: {
      uTime: { value: 0 },
      uScale: { value: 1 },
      uMarkColor: { value: new Color(style.markColor) },
      uMarkGrow: { value: style.markGrow },
      uMarkShape: { value: style.markShape === undefined ? -1 : SHAPE_CODE[style.markShape] },
      uDownColor: { value: new Color(style.downColor) },
      uDownGrow: { value: style.downGrow ?? 1.8 },
      uDownShape: { value: style.downShape === undefined ? -1 : SHAPE_CODE[style.downShape] },
    },
  });
}

/** One SHAPE_CODE number per dot. */
export function shapeArray(shapes: readonly DotShape[]): Float32Array {
  return Float32Array.from(shapes, (s) => SHAPE_CODE[s]);
}

/** Per-vertex attributes of a dots object; `mark` and `down` default to never, `shapes` to discs. */
export function dotGeometry(
  position: BufferAttribute,
  colors: Float32Array,
  sizes: Float32Array,
  mark?: Float32Array,
  down?: Float32Array,
  shapes?: Float32Array,
): BufferGeometry {
  const count = position.count;
  const geometry = new BufferGeometry();
  geometry.setAttribute('position', position);
  geometry.setAttribute('aColor', new BufferAttribute(colors, 3));
  geometry.setAttribute('aSize', new BufferAttribute(sizes, 1));
  geometry.setAttribute('aShape', new BufferAttribute(shapes ?? new Float32Array(count), 1));
  geometry.setAttribute('aMark', new BufferAttribute(mark ?? new Float32Array(count).fill(NEVER), 1));
  geometry.setAttribute('aDown', new BufferAttribute(down ?? new Float32Array(count).fill(NEVER), 1));
  return geometry;
}

export function dots(geometry: BufferGeometry, material: ShaderMaterial): Points {
  const points = new Points(geometry, material);
  points.frustumCulled = false;
  return points;
}

/** The satellites' scene positions, shared by every view that draws them. */
export function sharedPositions(count: number): BufferAttribute {
  const attribute = new BufferAttribute(new Float32Array(count * 3), 3);
  attribute.setUsage(DynamicDrawUsage);
  return attribute;
}

/** Laser links as segments that read the satellites' shared positions through an index. */
export function linkSegments(position: BufferAttribute, index: BufferAttribute, color: string, opacity: number): LineSegments {
  const geometry = new BufferGeometry();
  geometry.setAttribute('position', position);
  geometry.setIndex(index);
  const lines = new LineSegments(
    geometry,
    new LineBasicMaterial({ color, transparent: true, opacity, depthWrite: false }),
  );
  lines.frustumCulled = false;
  return lines;
}

const PATH_CAPACITY = 128;

/** Earth radii per CSS pixel along a route near the globe's front at the stories' default
 * zoom (a 32 degree view from about 3.5 radii): turns a dash pattern in pixels into one along
 * the line. */
export const RADII_PER_PX = 0.003;

/** A thick polyline whose points are rewritten every frame without new GPU buffers. */
export class ThickPath {
  readonly object: LineSegments2;
  readonly material: LineMaterial;
  private readonly buffer: InterleavedBuffer;

  constructor(color: string, widthPx: number, opacity = 1) {
    const geometry = new LineSegmentsGeometry();
    geometry.setPositions(new Float32Array(PATH_CAPACITY * 6));
    this.material = new LineMaterial({
      color,
      linewidth: widthPx,
      transparent: opacity < 1,
      opacity,
      depthWrite: opacity === 1,
    });
    this.object = new LineSegments2(geometry, this.material);
    this.object.frustumCulled = false;
    this.buffer = (geometry.attributes.instanceStart as InterleavedBufferAttribute).data;
    this.set([]);
  }

  /** Draw the polyline through `points` (x y z each); fewer than two points draws nothing. */
  set(points: readonly number[]): void {
    const n = Math.min(points.length / 3, PATH_CAPACITY + 1);
    const segments = Math.max(n - 1, 0);
    const a = this.buffer.array as Float32Array;
    for (let k = 0; k < segments; k++) {
      for (let j = 0; j < 6; j++) a[6 * k + j] = points[3 * k + j] as number;
    }
    this.object.geometry.instanceCount = segments;
    this.object.visible = segments > 0;
    this.buffer.needsUpdate = true;
  }

  /** Draw with this grey and dash pattern (CSS pixels, as in a chart; empty for solid). The
   * pattern is laid in Earth radii at RADII_PER_PX, so it holds at the default zoom; the line
   * keeps its width. Call layDashes when the points change. */
  setStyle(style: Pick<LineStyle, 'color' | 'dash'>): void {
    this.material.color.set(style.color);
    const dashed = style.dash.length >= 2;
    if (dashed) {
      this.material.dashSize = (style.dash[0] as number) * RADII_PER_PX;
      this.material.gapSize = (style.dash[1] as number) * RADII_PER_PX;
    }
    if (this.material.dashed === dashed) return;
    this.material.dashed = dashed;
    this.material.needsUpdate = true;
  }

  /** Lay the dashes along the points set now. It makes a new GPU buffer, so it is called when
   * the path itself changes, not every frame; the dashes then slide a little as it moves. */
  layDashes(): void {
    this.object.computeLineDistances();
  }
}

/** Points along the great circle from one site to another just above the ground (fibre). */
export function surfaceArc(a: readonly number[], b: readonly number[], radius: number): number[] {
  const [ax, ay, az] = a as [number, number, number];
  const [bx, by, bz] = b as [number, number, number];
  const angle = Math.acos(Math.min(1, Math.max(-1, (ax * bx + ay * by + az * bz) / (Math.hypot(ax, ay, az) * Math.hypot(bx, by, bz)))));
  const steps = Math.max(2, Math.ceil(angle / 0.03));
  const out: number[] = [];
  for (let i = 0; i <= steps; i++) {
    const f = i / steps;
    const wa = angle < 1e-9 ? 1 - f : Math.sin((1 - f) * angle) / Math.sin(angle);
    const wb = angle < 1e-9 ? f : Math.sin(f * angle) / Math.sin(angle);
    const x = wa * ax + wb * bx;
    const y = wa * ay + wb * by;
    const z = wa * az + wb * bz;
    const r = Math.hypot(x, y, z) / radius;
    out.push(x / r, y / r, z / r);
  }
  return out;
}
