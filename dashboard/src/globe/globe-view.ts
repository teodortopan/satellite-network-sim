// One WebGL renderer and one camera drawing one or more views of the globe. Each view is a
// page element; its scene is drawn into the part of the shared canvas under that element
// (viewport and scissor), so three side-by-side lanes cost one WebGL context, one upload of
// the shared satellite positions per frame, and one set of controls that turns them together.

import { PerspectiveCamera, Vector3, WebGLRenderer, type Scene } from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

export interface View {
  element: HTMLElement;
  scene: Scene;
  /** Called before the view is drawn with its size in device pixels. */
  beforeRender?: (widthPx: number, heightPx: number, pixelRatio: number) => void;
}

/** The camera distance (Earth radii from the centre) at which dots have their nominal size. */
const NOMINAL_DISTANCE = 3;

export class GlobeView {
  readonly renderer: WebGLRenderer;
  readonly camera = new PerspectiveCamera(32, 1, 0.01, 100);
  readonly controls: OrbitControls;
  private readonly views: View[] = [];
  private readonly resize: ResizeObserver;

  constructor(
    readonly host: HTMLElement,
    background: string,
  ) {
    const canvas = document.createElement('canvas');
    canvas.className = 'globe-canvas';
    canvas.setAttribute('aria-hidden', 'true');
    host.prepend(canvas);
    this.renderer = new WebGLRenderer({ canvas, antialias: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.setClearColor(background, 1);
    this.renderer.setScissorTest(true);
    this.controls = new OrbitControls(this.camera, host);
    this.controls.enablePan = false;
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.12;
    this.controls.rotateSpeed = 0.45;
    this.controls.zoomSpeed = 0.8;
    this.controls.minDistance = 1.25;
    this.controls.maxDistance = 9;
    this.resize = new ResizeObserver(() => {
      this.renderer.setSize(host.clientWidth, host.clientHeight, false);
    });
    this.resize.observe(host);
    this.renderer.setSize(host.clientWidth, host.clientHeight, false);
  }

  setViews(views: readonly View[]): void {
    this.views.splice(0, this.views.length, ...views);
  }

  /** Look at the globe from above `direction` (scene coordinates), `distance` radii out. */
  lookFrom(direction: readonly number[], distance: number): void {
    const d = new Vector3(direction[0], direction[1], direction[2]).normalize();
    this.camera.position.copy(d.multiplyScalar(distance));
    this.camera.lookAt(0, 0, 0);
    this.controls.update();
  }

  /** How much bigger dots are drawn than at the nominal distance (they grow as one zooms in). */
  get zoom(): number {
    return Math.sqrt(NOMINAL_DISTANCE / this.camera.position.length());
  }

  render(): void {
    this.controls.update();
    const canvasRect = this.renderer.domElement.getBoundingClientRect();
    const ratio = this.renderer.getPixelRatio();
    this.renderer.setScissor(0, 0, canvasRect.width, canvasRect.height);
    this.renderer.clear();
    for (const view of this.views) {
      const r = view.element.getBoundingClientRect();
      if (r.width < 1 || r.height < 1 || r.bottom < 0 || r.top > window.innerHeight) continue;
      const left = r.left - canvasRect.left;
      const bottom = canvasRect.bottom - r.bottom;
      this.renderer.setViewport(left, bottom, r.width, r.height);
      this.renderer.setScissor(left, bottom, r.width, r.height);
      this.camera.aspect = r.width / r.height;
      this.camera.updateProjectionMatrix();
      view.beforeRender?.(r.width * ratio, r.height * ratio, ratio);
      this.renderer.render(view.scene, this.camera);
    }
  }

  dispose(): void {
    this.resize.disconnect();
    this.controls.dispose();
    this.renderer.dispose();
    this.renderer.domElement.remove();
  }
}
