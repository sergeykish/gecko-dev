// |reftest| skip -- regexp-unicode-property-escapes is not supported
// Copyright 2020 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Regional_Indicator`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v13.0.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x01F1E6, 0x01F1FF]
  ]
});
testPropertyEscapes(
  /^\p{Regional_Indicator}+$/u,
  matchSymbols,
  "\\p{Regional_Indicator}"
);
testPropertyEscapes(
  /^\p{RI}+$/u,
  matchSymbols,
  "\\p{RI}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x01F1E5],
    [0x01F200, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Regional_Indicator}+$/u,
  nonMatchSymbols,
  "\\P{Regional_Indicator}"
);
testPropertyEscapes(
  /^\P{RI}+$/u,
  nonMatchSymbols,
  "\\P{RI}"
);

reportCompare(0, 0);
