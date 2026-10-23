// End-to-end fixture for tests/native/react_hmr_e2e_test.cpp (served by
// `jsui dev`; e2e.test.js copies this directory and the test edits the copy).
import { AppRegistry } from 'react-native';
import { Host } from '@orktes/jsui-react';
import App from './App';

Host.postMessage('boot', 1);
AppRegistry.registerComponent('main', () => App);
