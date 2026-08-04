.. _ttzp_release_process:

Release Process
###############

This page is intended for Release Managers and describes the release process used by Tenstorrent
firmware.

Overview
********

The following steps are required to be followed by firmware release engineers when creating a new
Tenstorrent firmware release.

The release process consists of the following high level steps:

1. Create a release branch from main, and increment the version on main to the next minor version.

2. Create a release candidate (RC) from the release branch and post it to GitHub.

3. Announce the RC via internal channels, providing a link to the GitHub release page.

4. Sync with qualification team to kick off qualification testing on the RC.

5. Backport any critical fixes to the release branch.

6. Create new RCs as needed until the RC is validated by the qualification team.

7. Once the final RC has been validated, create a final release and post it to GitHub.

8. Announce the release via internal channels, providing a link to the GitHub release page.


Shared Building Blocks
**********************

The following procedures are shared across the RC and final release flows described in
`Release Flow`_. They are documented once here and referenced from the individual steps.

Working on a Protected Release Branch
=====================================

Release branches (``vX.Y-branch``) are protected by the same repository ruleset
as ``main``. This means you **cannot** push commits directly to a release
branch: every change, including version bumps, must go through a pull request
with at least one approving review. Branch deletion and force pushes are also
blocked.

Since the base of the PR is the protected release branch, your commits have to
live somewhere else. Cut a **topic branch** from the release branch, push it to
your fork, and open a PR whose base is the release branch. Avoid starting the
topic branch name with ``vX.Y``, even on your fork. Put the version at the end
instead, as in ``<topic>-vX.Y``:

.. code-block:: shell

   # Start from an up-to-date copy of the release branch
   git fetch upstream
   git checkout -b rc2-rev-bump-v19.9 upstream/v19.9-branch

   # Make the change (e.g. the RC version bump), which creates the commits
   ./scripts/update_versions.sh update-rc

   # Push the topic branch to your fork and open the PR against the release branch
   git push -u origin rc2-rev-bump-v19.9
   gh pr create --base v19.9-branch --repo tenstorrent/tt-system-firmware

Here ``upstream`` is the main tt-system-firmware repository and ``origin`` is
your personal fork; adjust the remote names to match your setup. Note that
``update_versions.sh`` commits to whatever branch you currently have checked out,
so create the topic branch *before* running it.

Two consequences worth remembering:

* Creating the release branch for the first time is fine - the protection applies
  to updating an existing branch, not to creating it. This is why
  ``scripts/create-release-candidate.sh`` can push the new branch directly.

* Tags are not covered by the ruleset, so signed release and RC tags are pushed
  directly. Always tag a commit that is already merged on the release branch, not
  a local-only commit.

Signed Tags and Immutable Releases
==================================

Tenstorrent firmware uses signed tags and immutable releases to ensure the
integrity of each release. In order to create a signed tag, you must have a
signing key configured in Git. You can verify that your signing key is
configured correctly by running the following commands

.. code-block:: shell

   # Path to signing key should be returned
   git config --global user.signingkey
   # Should return "ssh". GPG keys are supported,
   # but the process is not documented in this guide.
   git config --global gpg.format
   # Provide github CLI with access to the signing keys in your account
   gh auth refresh -h github.com -s admin:ssh_signing_key
   # Check that you have a signing key available (type MUST be signing)
   gh ssh-key list
   # Check that the signing key you are using matches the key in your account
   cat $(git config --global user.signingkey).pub

You can verify that a tag has been signed correctly by running ``git show`` for
the tag, which should show a signature block in the output (look for the ``BEGIN
PGP SIGNATURE`` or ``BEGIN SSH SIGNATURE`` marker in the output):

For example:

.. code-block:: shell

   tag v19.7.0
   Tagger: Scott Lindsay <slindsay@tenstorrent.com>
   Date:   Wed Mar 11 18:43:17 2026 -0400

   tt-system-firmware 19.7.0
   -----BEGIN PGP SIGNATURE-----

GitHub will show a "Verified" badge for tags that have been signed with a key
that is associated with a GitHub account, and the signature is valid. For
more details, see here:
`GitHub Docs - About commit signature verification <https://docs.github.com/en/authentication/managing-commit-signature-verification/about-commit-signature-verification>`_.

Posting a Release to GitHub
===========================

Pushing a release tag (any tag starting with ``v``) to GitHub triggers the
release workflow (``.github/workflows/release.yml``), which builds the release
artifacts and creates a **draft** release for that tag at `GitHub Releases
<https://github.com/tenstorrent/tt-system-firmware/releases>`_. The draft is not
visible to users until you publish it. Once the workflow has finished and the
draft release exists, complete the following steps:

1. Edit the release. The workflow marks every draft as a pre-release, so an RC
   needs no change here. For a final release, uncheck "Set as a pre-release" and
   make sure "Set as the latest release" is checked.

2. Verify the release includes a verified tag. There should be a green
   "Verified" badge next to the tag in the release, indicating that the tag is
   signed with a key that is associated with a GitHub account, and the signature is
   valid. If the tag is not verified, do not publish the release and investigate
   the issue.

3. For final releases, copy the contents of the associated release notes template
   (e.g. ``doc/release/release-notes-19.9.md``) into the release notes area.

4. Publish the release. **Note:** since we use immutable releases, once a release
   is published, it cannot be edited.

5. Announce the release via internal channels, providing a link to the
   GitHub release page. Release notes are not required for RCs, but it
   is recommended to link the draft release notes template for the release (e.g.
   ``doc/release/release-notes-19.9.md``) in the RC announcement.

Release Flow
************

This is the sequential process for taking a release from an initial release
candidate through to a validated final release. Each step below references the
`Shared Building Blocks`_ where appropriate.

Creating the Release Branch and First RC
========================================

Firmware follows a release candidate (RC) process. The RC process is used to
allow for testing and validation of firmware before a final release is made.
Each RC is tagged and posted to GitHub as a pre-release, and the final release
is tagged and posted to GitHub as a stable release.

There are two ways to create the release branch and first RC: an automated
script (recommended for the common case), or the manual steps below (useful for
understanding what the script does, or when a step needs manual intervention).
Both paths produce the same result.

Option 1: Automated (Recommended)
---------------------------------

.. note::

   The steps below require the GitHub CLI tool to be installed and
   authenticated. You can verify that you are authenticated by running
   ``gh auth status`` and checking that you have access to the repository.
   Installation instructions for the GitHub CLI can be found here:
   `GitHub CLI - Installation <https://github.com/cli/cli#installation>`_

The manual steps below can be automated using the
``scripts/create-release-candidate.sh`` script, which will create the release
branch, increment the version on main, tag the RC, and push the changes to
GitHub. For example:

.. code-block:: shell

   # You can specify a different remote (i.e. a personal fork) in order to test
   # the script without pushing to the main tt-system-firmware repository.  You
   # can also specify the --dry-run flag to create all relevant branches and
   # tags locally without pushing to GitHub.
   ./scripts/create-release-candidate.sh git@github.com:tenstorrent/tt-system-firmware.git

Once the script has pushed the branch and RC tag, follow
`Posting a Release to GitHub`_ to publish the RC as a pre-release.

Option 2: Manual Steps
----------------------

To start the RC process manually, perform the following steps:

1. Release Branch Creation - Create a release branch from main with the name
   ``vX.Y-branch`` where X and Y are the major and minor version numbers of the
   release.

2. Main Version Bump - Immediately after creating the release branch, post a PR
   incrementing the next minor version on main, and merge after CI is
   successful. For example, if the release branch is ``v19.9-branch``, then the
   version on main should be incremented to 19.10. This step can be performed
   using the command ``./scripts/update_versions.sh post-branch``, then creating
   a PR with the commits it creates. Do this as soon as the branch exists, so
   that commits merged to main after the branch point are not stamped with the
   release's version number. This PR can proceed in parallel with the RC steps
   below; it does not need to block them.

3. RC Version Update - Update the MINOR version field, and set the EXTRAVERSION
   field to "rc1" in the release branch. For example, if the release branch is
   ``v19.9-branch``, then the MINOR version should be set to 9 and the
   EXTRAVERSION field should be set to "rc1". This step can be performed using
   the command ``./scripts/update_versions.sh rc``.

4. RC Tagging - Tag the release branch with the first RC tag, following the
   format ``vX.Y.Z-rc1`` where X, Y and Z are the major, minor and patch version
   numbers of the release. For example, if the release branch is
   ``v19.9-branch``, then the first RC tag should be ``v19.9.0-rc1``. The tag can
   be created with the command
   ``git tag -s vX.Y.Z-rc1 -m "tt-system-firmware vX.Y.Z-rc1"``, where
   X, Y and Z are the major, minor and patch version numbers of the release.

5. Push to GitHub - Push the release branch and RC tag to GitHub. This will start
   the CI process for the release from the tag. Once this is complete, follow
   `Posting a Release to GitHub`_ steps to post the RC to GitHub as a published
   pre-release.

Backports and Validation
========================

Once the RC is posted to GitHub, it is the responsibility of the release manager
to work with the qualification team to validate the RC. During the RC process,
the following changes are candidates for backporting to the release branch:

.. Note::

   No changes (except version increments) should be made to the release branch
   without also being made to main.

* Critical bug fixes that are required for the RC to pass qualification testing.
  These should be backported to the release branch and included in the next RC.

* Documentation updates for features included in the release. These do not
  require creation of a new RC for validation.

* Features, at the discretion of the release manager and qualification team.

Backports should be made via PRs against the release branch. Any change that is
not documentation updates should trigger a new RC to be created for validation.

Creating Additional RCs
=======================

To create a new RC for validation after the first RC, follow these steps:

1. RC Version Update - Update the EXTRAVERSION field in the release branch to the
   next RC version (e.g. "rc2", "rc3", etc). This step can be performed using the
   command ``./scripts/update_versions.sh update-rc``.

2. Merge Version Bump - Put the version commits on a topic branch cut from the
   release branch (e.g. ``v19.9.0-rc2-rev-bump``), push it to your fork, and open
   a PR with the base set to the release branch. Merge after CI is successful.
   See `Working on a Protected Release Branch`_.

3. RC Tagging - Once the PR is merged, check out the release branch and pull the
   merge commit, then tag it with the new RC tag, following the format
   ``vX.Y.Z-rcN`` where X, Y and Z are the major, minor and patch version numbers
   of the release, and N is the RC version number. For example, if the release
   branch is ``v19.9-branch`` and this is the second RC, then the new RC tag should
   be ``v19.9.0-rc2``. The tag can be created with the command
   ``git tag -s vX.Y.Z-rcN -m "tt-system-firmware vX.Y.Z-rcN"``, where X, Y and Z are
   the major, minor and patch version numbers of the release, and N is the RC version number.

4. Push to GitHub - Push the new RC tag to GitHub. This will start the CI process
   for the release from the new RC tag. Once this is complete, follow
   `Posting a Release to GitHub`_ steps to post the new RC to GitHub as a
   published pre-release.


Creating the Final Release
==========================

Once a final RC has been validated by the qualification team, the release
manager can create the final release. To create the final release, follow these
steps:

1. Version Finalization - Update the EXTRAVERSION field in the release branch to
   be empty. This step can be performed using the command
   ``./scripts/update_versions.sh pre-release``.

2. Merge Version Bump - Put the version commits on a topic branch cut from the
   release branch (e.g. ``v19.9.0-version-finalization``), push it to your fork,
   and open a PR with the base set to the release branch. Merge after CI is
   successful. See `Working on a Protected Release Branch`_.

3. Release Tagging - Once the PR is merged, check out the release branch and pull
   the merge commit, then tag it with the final release tag, following the format
   ``vX.Y.Z`` where X, Y and Z are the major, minor and patch version numbers of
   the release. For example, if the release branch is ``v19.9-branch``, then the
   final release tag should be ``v19.9.0``. The tag can be created with the
   command ``git tag -s vX.Y.Z -m "tt-system-firmware vX.Y.Z"``, where X, Y and Z
   are the major, minor and patch version numbers of the release.

4. Push to GitHub - Push the final release tag to GitHub (the release branch
   itself is already up to date via the merged PR in step 2). This will start the
   CI process for the release from the final release tag. Once this is complete,
   follow `Posting a Release to GitHub`_ steps to post the final release to
   GitHub as a published stable release.

Testing the Release Process in a Fork
*************************************

The release process can be tested by creating a release candidate from a release
branch in a personal fork of the repository, and posting the RC to GitHub as a
pre-release. This allows you to verify that the RC creation and posting process
works correctly, without affecting the main tt-system-firmware repository.

To do so, follow the steps in the `Creating the Release Branch and First RC`_
section, but push the release branch and RC tag to a personal fork of the
repository instead of the main tt-system-firmware repository. For example,

.. code-block:: shell

   # Run automated RC creation script, specifying your personal fork as the remote
   ./scripts/create-release-candidate.sh git@github.com:<username>/tt-system-firmware.git

Note that the release process requires a few secrets to function correctly.
These can be added within the "Settings > Secrets and variables > Actions" page
of the repository on GitHub. For testing purposes, you can add these secrets to
your personal fork as well.

* ``SIGNATURE_KEY``: This should be the private key used to sign DMFW and CMFW
   during release. To generate, run
   ``openssl genrsa -out private_key.pem 2048 && base64 -i private_key.pem``

* ``PKG_SIGNING_KEY_DEB``: This should be an ASCII-armored GPG key used to
   sign Debian packages during release. To generate, run
   ``gpg --gen-key`` and follow the prompts to create a new GPG key, then
   export the key with ``gpg --armor --export <key-id>``. Note: the key
   cannot have a passphrase set- just leave the passphrase blank when creating
   the key.
