// Worker thread of react-compiler.js: compiles one file per message.
import { parentPort } from 'node:worker_threads';
import { compileInProcess } from './react-compiler.js';

parentPort.on('message', async ({ id, source, file, o }) => {
  try {
    parentPort.postMessage({ id, result: await compileInProcess(source, file, o) });
  } catch (e) {
    parentPort.postMessage({ id, error: e.message, stack: e.stack });
  }
});
