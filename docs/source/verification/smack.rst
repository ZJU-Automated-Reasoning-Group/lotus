SMACK Verification Frontend
===========================

SMACK translates LLVM bitcode into Boogie programs for verifier backends. Lotus
keeps the migrated SMACK implementation under ``third-party/verification/smack/`` and
the command-line frontend under ``tools/verifier/smack/``.

The upstream SMACK documentation has been copied into this repository under
``docs/source/verification/smack/`` so it can be updated alongside the migrated
frontend.

Copied Documentation
--------------------

.. image:: smack/smack-logo.png
   :alt: SMACK logo
   :width: 180px

* :download:`Installation <smack/installation.md>`
* :download:`Running SMACK <smack/running.md>`
* :download:`Usage notes <smack/usage-notes.md>`
* :download:`Boogie code generation <smack/boogie-code.md>`
* :download:`FAQ <smack/faq.md>`
* :download:`Demos <smack/demos.md>`
* :download:`Projects <smack/projects.md>`
* :download:`Publications <smack/publications.md>`
* :download:`People <smack/people.md>`
* :download:`Code of conduct <smack/code-of-conduct.md>`

Build Targets
-------------

* ``SmackUtils`` / ``SmackTranslator`` - migrated SMACK library targets
* ``llvm2bpl`` - LLVM-to-Boogie translator
* ``smack`` - frontend script installed with the verifier tools

See Also
--------

* :doc:`clam` - CLAM abstract interpretation frontend
* :doc:`seahorn` - SeaHorn verification frontend
* :doc:`../user_guide/verification_backends` - verification backend overview
