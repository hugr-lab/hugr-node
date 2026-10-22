# Spec 019: `CLUSTER UPDATE EXTENSION` keeps what it does not name

- **Status**: implemented
- **Date**: 2026-10-22
- **Author**: hugr lab (found by hugr-platform spec 010's review - the console's update form inherits it; the owner,
  2026-10-22: fix it)

## Problem

An update rewrote two parts of the item it did not name:

- **The repository** was resolved again with no `FROM`, and the grammar took none for an update. On a node trusting
  one repository the item silently moved to it. On a node trusting several, every update was refused with "name the
  repository with FROM" - which the statement could not do.
- **The comment** was written empty: the upsert takes the statement's comment, and an update without `COMMENT` has
  none.

## Design

- `CLUSTER UPDATE EXTENSION <name> VERSION '<v>' [FROM <repository>] [IN GROUP <group>] [COMMENT '<text>']`:
  - without `FROM`, the item's stored repository - which must still be one this node trusts (else the refusal names
    it and asks for `FROM`);
  - with `FROM`, that repository, checked as an install's;
  - without `COMMENT`, the stored comment; with it, the new one.
- An install is unchanged: without `FROM` only on a node trusting exactly one repository.

## Tests

`test/cpp/test_acl_cluster_install.cpp`:
- an update with no `FROM` keeps the item's repository and its comment;
- with a second repository trusted, an update with no `FROM` keeps the stored one, and is not refused;
- `FROM` an untrusted repository is refused, writing nothing;
- `COMMENT` replaces the comment.
