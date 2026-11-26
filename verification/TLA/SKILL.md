---
name: verify-mex-tla
description: Run and maintain MEX bounded specifications when changing matching, orders, replication, gateway, queues, or test tools.
---

Read the applicable row in [README.md](README.md) and its entry in
[catalog.json](catalog.json). Use the listed source paths to identify checks.

Run the selected check with the command in README.md. Use the statechart
skill when you need to inspect a model counterexample.

Read the TLC log to identify the first violated property. Replay an equivalent
sequence through the real component. Record whether the model or implementation
needs correction. Keep a reproducible regression case when the implementation
breaks a property. Check that the mutation still fails for the named reason
after updating a model. Update the coverage and its source baseline when you
establish that the model describes changed code.
