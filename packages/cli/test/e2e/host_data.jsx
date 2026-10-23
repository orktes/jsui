// End-to-end fixture for tests/native/react_host_data_e2e_test.cpp: an
// inventory screen fed by shared host state (useHostState / jsui_set_state /
// jsui_patch_state), a buy button that asks the game (Host.call ->
// jsui_resolve_call / jsui_reject_call), a UI-owned setting (HostState.set ->
// on_state) and a game -> UI call (jsui_call_ui -> Host.handle). Every
// commit posts what it rendered as `render`. 400 x 300.
import React, { memo, useEffect, useState } from 'react';
import { AppRegistry, Pressable, Text, View } from 'react-native';
import { Host, HostState, useHostHandler, useHostState } from '@orktes/jsui-react';

let rowRenders = 0;
const Row = memo(function Row({ id, item }) {
  rowRenders++;
  return <Text>{`${id} x${item.count}`}</Text>;
});

const Wallet = memo(function Wallet() {
  const gold = useHostState('gold', 0);
  useEffect(() => Host.postMessage('gold', gold), [gold]);
  return <Text>{`${gold} gold`}</Text>;
});

const Inventory = memo(function Inventory() {
  const inv = useHostState('inventory', { items: {} });
  const ids = Object.keys(inv.items);
  useEffect(() => {
    Host.postMessage('inventory', { ids, rowRenders });
  });
  return (
    <View>
      {ids.map((id) => (
        <Row key={id} id={id} item={inv.items[id]} />
      ))}
    </View>
  );
});

function App() {
  const [status, setStatus] = useState('idle');
  const [late, setLate] = useState(false);
  useHostHandler('ui.prompt', async ({ text }) => {
    setStatus('prompt:' + text);
    await new Promise((r) => setTimeout(r, 50));
    return { answer: 'yes', text };
  });
  useEffect(() => {
    Host.handle('ui.mountLate', () => {
      setLate(true);
      return true;
    });
    Host.postMessage('ready', true);
  }, []);
  const buy = (item) =>
    Host.call('shop.buy', { item }, { timeout: 2000 }).then(
      (r) => Host.postMessage('bought', r),
      (e) => Host.postMessage('buyFailed', { message: e.message, code: e.code || null }),
    );
  useEffect(() => {
    if (status !== 'idle') Host.postMessage('status', status);
  }, [status]);
  return (
    <View style={{ flex: 1 }}>
      <Pressable style={{ height: 40 }} onPress={() => buy('potion')} />
      <Pressable style={{ height: 40 }} onPress={() => HostState.set('settings', { volume: 0.25 })} />
      <Wallet />
      <Inventory />
      {late ? <Wallet /> : null}
    </View>
  );
}

AppRegistry.registerComponent('host_data', () => App);
