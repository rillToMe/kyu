#ifndef INTEL_GEN12_GHAL_H
#define INTEL_GEN12_GHAL_H

// ============================================================
// GHAL-level integration check — AL-14
// Uses ONLY public ghal_* API (the compositor's view): create →
// fill → blit → present → destroy. Proves the dispatch path
// window→GHAL→backend is wired; pixel proof lives in the AL-9/10
// backend tests. No PCI/MMI/gen knowledge here by construction.
// ============================================================

void gen12_ghal_check(void);

#endif // INTEL_GEN12_GHAL_H
