// The UI of the Three.js engine demo (site/demo/): the engine examples'
// shared HUD (integrations/shared/ui/app.jsx, registered as 'App'), which the
// page renders into a texture surface on Three.js' WebGPU device and
// composites over its frame. Hosts without the Radar native component (this
// one) leave the radar out.
import { AppRegistry } from 'react-native';
import '../../integrations/shared/ui/app.jsx';

// The main surface is not shown (the page makes only the texture surface).
AppRegistry.registerComponent('Main', () => () => null);
