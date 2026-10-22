import React from 'react';
import { Column, SectionHead } from '../components/ui.jsx';

export function HomePage() {
  return (
    <Column style={{ paddingTop: 64 }}>
      <SectionHead eyebrow="Home" title="Home" lead="Coming soon." />
    </Column>
  );
}
