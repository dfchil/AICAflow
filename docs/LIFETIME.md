# AICAflow DKR Edition: lifetime

There are three separate lifetimes: a bank's samples, a flow instance and a
source sample's loop points. They must not be conflated.

An AFB1 bank owns uploaded samples and compact flow tables. An active instance
retains its flow and bank. Call `afx_instance_stop`, keep calling `afx_update`,
observe completion with `afx_instance_status`, then call
`afx_instance_recycle`. Only after every instance using a bank has recycled may
`afx_sfx_bank_release` free it.

At a scene transition, cancel delayed DKR requests, stop active SFX, wait for
their recycling, then release scene-local and idle fallback banks. Resident
music samples and core SFX remain loaded; a new song only replaces its control
flow.

N64 sample looping is not a request for an infinite AICAflow instance. The N64
importer marks an AICAflow SFX as controlled only when its envelope sustains
indefinitely. A finite envelope gets authored `KEYOFF` and `END`, even if its
sample has loop points. Therefore normal pickup and UI sounds complete without
per-sound host timeouts.

`PARK` is for a genuinely controlled flow that awaits an explicit stop. It is
not completion. Do not release, reuse or patch an instance merely because it
is parked; stop it and wait for normal completion/recycling.

The AICA has finite RAM, channels and executor budget. A failed fallback-bank
allocation may be retried after idle banks or a speculative music control flow
are released. Format and file errors are permanent for that request.
