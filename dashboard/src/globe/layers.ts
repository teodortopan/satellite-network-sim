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
  attribute float aMark;
  attribute float aDown;
  uniform float uTime;
  uniform float uScale;
  uniform vec3 uMarkColor;
  uniform float uMarkGrow;
  uniform vec3 uDownColor;
  varying vec3 vColor;
  void main() {
    vec4 mv = modelViewMatrix * vec4(position, 1.0);
    gl_Position = projectionMatrix * mv;
    float size = aSize;
    vColor = aColor;
    if (uTime >= aDown) {
      vColor = uDownColor;
      size *= 1.8;
    } else if (uTime >= aMark) {
      vColor = uMarkColor;
      size *= uMarkGrow;
    }
    gl_PointSize = size * uScale;
  }
`;

const DOT_FRAGMENT = /* glsl */ `
  varying vec3 vColor;
  void main() {
    vec2 d = gl_PointCoord - 0.5;
    if (dot(d, d) > 0.25) discard;
    gl_FragColor = vec4(vColor, 1.0);
  }
`;

export interface DotStyle {
  markColor: string;
  markGrow: number;
  downColor: string;
}

/**
 * Round dots whose colour switches at a time: to the mark colour from aMark on (a satellite
 * that learned the news, a detector), to the down colour from aDown on (a failed element).
 * Times are story seconds; the shader compares them with uTime and computes nothing else.
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
      uDownColor: { value: new Color(style.downColor) },
    },
  });
}

/** Per-vertex attributes of a dots object; `mark` and `down` default to never. */
export function dotGeometry(
  position: BufferAttribute,
  colors: Float32Array,
  sizes: Float32Array,
  mark?: Float32Array,
  down?: Float32Array,
): BufferGeometry {
  const count = position.count;
  const geometry = new BufferGeometry();
  geometry.setAttribute('position', position);
  geometry.setAttribute('aColor', new BufferAttribute(colors, 3));
  geometry.setAttribute('aSize', new BufferAttribute(sizes, 1));
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

  /** Dashed (a broken route) or solid; dash and gap lengths in Earth radii. */
  setDashed(dashed: boolean, dash = 0.014, gap = 0.012): void {
    this.material.dashSize = dash;
    this.material.gapSize = gap;
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
