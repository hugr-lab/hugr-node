# Spec 046: a secret made a moment ago is the node's at once

- **Status**: implemented
- **Date**: 2026-11-26
- **Author**: the owner (2026-11-26: the node should refresh its secrets list itself when it applies the policy),
  hugr-platform spec 014

## Summary

A node's secrets service (tresor) keeps the list of the secrets the node may use for 30 s (tresor spec 004). A secret
the operator makes is therefore no match for up to 30 s, and an ATTACH naming it is refused in that window with "no
secret … the node may use". hugr-platform's demo met this when it attached platform B right after making B's link
secret (its spec 014, check 18).

The node now asks each attached secrets service for its list again before it relies on a secret it may not see yet:
- before a source that names a secret is attached: by the profile write's hot apply on the writing node, by the tick's
  hot apply on every other node, and by a lazy source's first attach;
- in the `hugr` type, when the secret it reads at a connect is no match, before it refuses (once).

## Design

- **`RefreshSecretsLists(db)`** (acl_cluster.cpp) runs `SELECT count(*) FROM <service>.secrets()` on its own
  connection for every attached database of type `tresor`. tresor's `secrets()` always fetches a fresh list and
  refreshes the cache (tresor spec 004).
  - A service that cannot list keeps its last list, by tresor's own rule.
  - Nothing here fails: the attach or the read that follows says what is missing.
- **Where it runs:**
  - the write's hot apply (`CLUSTER ATTACH … SECRET s` on the writing node), before its ATTACH;
  - `ApplyHot` (the tick) for a source item naming a secret;
  - `AttachLazySource` for such an item;
  - `Pool::ReadToken` (the `hugr` type), on a miss, then the lookup once more.

  The bootstrap needs none: it attaches tresor at step 2, so its list is fresh at step 6.
- **The cost**: one list request per attach of a source that names a secret, which happens rarely, and one per missed
  lookup in the `hugr` type. A secret that does not exist at all still costs a list request per connect attempt.
  Those attempts are bounded by the pool's own retry and the statements that read the link.
- **The read is the node's**: on its own connection, no session, so tresor lists what the node may use.

## Security

Nothing new is read or granted. The node asks for the list it already holds, and tresor decides what is in it. No
secret value is listed (descriptors only, tresor spec 004).

## Testing

- The federation e2e (`test/e2e/federation/tresor.sh`), step 4:
  1. A lists its secrets;
  2. the operator makes a new `hugr` secret;
  3. A attaches B by it at once and reads.

  Without the change, A refuses: `hugr: b2: no secret "b_link2" the node may use` (run 2026-11-26).
- The profile's paths (the write's and the tick's hot apply, the lazy attach) run against hugr-platform's demo (its
  check 18). That check retries the ATTACH for 90 s until a hugr-node release carries this spec, and drops the retry
  then.

## Alternatives considered

- **In tresor: a miss refreshes the list.** That would serve every consumer. But tresor is a product of its own, and
  its 30 s list is its rule. The owner placed the refresh in the node, where the policy is applied.
- **A shorter list cache.** It would cost every lookup on every node, for a write that happens rarely.
