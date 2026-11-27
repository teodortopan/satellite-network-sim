// One globe in one page element, for the stories that show a single view: the renderer, the
// scene, and the per-frame settings every dots object and thick line needs (dot size with the
// zoom, line width in pixels).

import { BufferAttribute, Color, LineSegments, Mesh, Points, Scene, Vector3, type BufferGeometry, type Material, type Object3D, type ShaderMaterial } from 'three';
import { THEME, type DotShape } from '../theme';
import { raySphere, sceneToLatLon } from './coords';
import { GlobeView } from './globe-view';
import { dotGeometry, dotMaterial, dots, earth, shapeArray, ThickPath } from './layers';

export class Stage {
  readonly globe: GlobeView;
  readonly scene = new Scene();
  private readonly materials: ShaderMaterial[] = [];
  private readonly paths: ThickPath[] = [];

  constructor(readonly element: HTMLElement) {
    this.globe = new GlobeView(element, THEME.night);
    this.scene.add(earth({ earth: THEME.earth, land: THEME.land }));
    this.globe.setViews([
      {
        element,
        scene: this.scene,
        beforeRender: (w, h, ratio) => {
          for (const p of this.paths) p.material.resolution.set(w, h);
          for (const m of this.materials) (m.uniforms.uScale as { value: number }).value = ratio * this.globe.zoom;
        },
      },
    ]);
  }

  add(...objects: Object3D[]): void {
    this.scene.add(...objects);
  }

  /** A dots material whose size follows the zoom (marks and failures are not used here). */
  dotMaterial(): ShaderMaterial {
    const m = dotMaterial({ markColor: THEME.news, markGrow: 1, downColor: THEME.failed });
    this.materials.push(m);
    return m;
  }

  /** A thick line added to the scene, drawn above the satellites. */
  path(color: string, widthPx: number, opacity = 1): ThickPath {
    const p = new ThickPath(color, widthPx, opacity);
    p.object.renderOrder = 2;
    this.paths.push(p);
    this.scene.add(p.object);
    return p;
  }

  /** Fixed dots at scene positions (x y z each), one colour, size and shape per dot. */
  fixedDots(positions: readonly number[], colors: readonly string[], sizes: readonly number[], shapes: readonly DotShape[]): Points {
    const position = new BufferAttribute(new Float32Array(positions), 3);
    const geometry = dotGeometry(position, rgbArray(colors), new Float32Array(sizes), undefined, undefined, shapeArray(shapes));
    const points = dots(geometry, this.dotMaterial());
    points.renderOrder = 1;
    return points;
  }

  render(): void {
    this.globe.render();
  }

  /**
   * The latitude and longitude under a point of the window (clientX, clientY), or null off the
   * Earth: a ray from the camera through that point to the sphere of the Earth (radius 1).
   * The camera orbits a fixed Earth, so the scene's Earth-fixed axes hold whatever the turn.
   */
  pick(clientX: number, clientY: number): { lat_deg: number; lon_deg: number } | null {
    const r = this.element.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) return null;
    const camera = this.globe.camera;
    camera.aspect = r.width / r.height;
    camera.updateProjectionMatrix();
    camera.updateMatrixWorld();
    const ndc = new Vector3(((clientX - r.left) / r.width) * 2 - 1, -(((clientY - r.top) / r.height) * 2 - 1), 0.5);
    const direction = ndc.unproject(camera).sub(camera.position);
    const hit = raySphere(camera.position.toArray(), direction.toArray());
    return hit === null ? null : sceneToLatLon(...hit);
  }

  dispose(): void {
    this.scene.traverse((o) => {
      // LineSegments2 is a Mesh.
      if (o instanceof Mesh || o instanceof Points || o instanceof LineSegments) {
        const { geometry, material } = o as { geometry: BufferGeometry; material: Material | Material[] };
        geometry.dispose();
        for (const m of Array.isArray(material) ? material : [material]) m.dispose();
      }
    });
    this.globe.dispose();
  }
}

/** RGB per satellite from one colour each. */
export function rgbArray(colors: readonly string[]): Float32Array {
  const out = new Float32Array(colors.length * 3);
  const c = new Color();
  colors.forEach((hex, i) => {
    c.set(hex);
    out.set([c.r, c.g, c.b], 3 * i);
  });
  return out;
}
