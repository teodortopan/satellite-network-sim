// One globe in one page element, for the stories that show a single view: the renderer, the
// scene, and the per-frame settings every dots object and thick line needs (dot size with the
// zoom, line width in pixels).

import { BufferAttribute, Color, LineSegments, Mesh, Points, Scene, type BufferGeometry, type Material, type Object3D, type ShaderMaterial } from 'three';
import { THEME } from '../theme';
import { GlobeView } from './globe-view';
import { dotGeometry, dotMaterial, dots, earth, ThickPath } from './layers';

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

  /** A dots material whose size follows the zoom. */
  dotMaterial(markColor: string = THEME.news, downColor: string = THEME.failed): ShaderMaterial {
    const m = dotMaterial({ markColor, markGrow: 1, downColor });
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

  /** Fixed dots at scene positions (x y z each), one colour and size per dot. */
  fixedDots(positions: readonly number[], colors: readonly string[], sizes: readonly number[]): Points {
    const geometry = dotGeometry(new BufferAttribute(new Float32Array(positions), 3), rgbArray(colors), new Float32Array(sizes));
    const points = dots(geometry, this.dotMaterial());
    points.renderOrder = 1;
    return points;
  }

  render(): void {
    this.globe.render();
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
