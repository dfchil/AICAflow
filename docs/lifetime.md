# Lifetime

[Documentation](README.md)

An AFB owns one contiguous AICA sample allocation. An uploaded AFX flow retains
that bank; an active instance retains its flow. Release in this order:

1. Stop an active instance and wait for `AFX_DONE` or `AFX_ERROR`.
2. Recycle the instance and wait until its handle is stale.
3. Free the AFX flow with `afx_asset_free()`.
4. Release the bank with `afx_bank_release()`.

Do not release a bank while a flow or instance refers to it. A scene can keep a
bank resident and upload/release many small AFX control streams against it.
Finite sounds must author `KEYOFF` and `END`; `PARK` is only for a controlled
flow that the SH4 will explicitly stop.
