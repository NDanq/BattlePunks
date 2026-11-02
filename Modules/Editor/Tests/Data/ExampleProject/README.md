Tiny is the example NimbleEditorProjectTests unpacks with Engine/Examples/UnpackExample.cmake.
Its archives are in ../Examples, packed by Engine/Examples/pack_example.py:

    python3 Engine/Examples/pack_example.py Engine/Modules/Editor/Tests/Data/ExampleProject/Tiny --out Engine/Modules/Editor/Tests/Data/Examples

Its Saved/, Build/ and Tools/__pycache__/ hold junk on purpose, to check that
the packer leaves them out; the test makes them, they are not tracked.
