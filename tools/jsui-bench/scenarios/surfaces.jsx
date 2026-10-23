// jsui-bench "surfaces": one bundle, many root views (jsui_surface_create).
// The bench creates 30 `Plate` surfaces (nameplates: name, HP bar, level) and,
// depending on the scenario, sets every surface's props each frame: with HP
// changing (surfaces-props), with the same props (surfaces-same-props, a
// host that pushes its state every frame without diffing), one surface's
// changing per frame (surfaces-one-props), or resizes every surface each
// frame (surfaces-resize).
import React from 'react';
import { AppRegistry, StyleSheet, Text, View, useWindowDimensions } from 'react-native';

function Plate({ name, hp, level }) {
  const { width } = useWindowDimensions();
  return (
    <View style={styles.plate}>
      <View style={styles.row}>
        <Text style={styles.level}>{level}</Text>
        <Text style={styles.name} numberOfLines={1}>
          {width > 100 ? name : name.slice(0, 5)}
        </Text>
      </View>
      <View style={styles.bar}>
        <View style={[styles.fill, { width: `${hp}%` }]} />
      </View>
    </View>
  );
}

AppRegistry.registerComponent('Main', () => () => <View style={{ flex: 1 }} />);
AppRegistry.registerComponent('Plate', () => Plate);

const styles = StyleSheet.create({
  plate: { flex: 1, padding: 4, backgroundColor: '#0d1322cc', borderRadius: 6 },
  row: { flexDirection: 'row', alignItems: 'center' },
  level: { color: '#ffd166', fontSize: 11, fontWeight: '700', marginRight: 4 },
  name: { color: '#fff', fontSize: 12 },
  bar: { height: 6, marginTop: 4, borderRadius: 3, backgroundColor: '#3a1d24', overflow: 'hidden' },
  fill: { height: 6, backgroundColor: '#ef476f' },
});
