# `catalejo-macro`

`catalejo-macro` supplies the `Field` derive used for typed traversal of foreign
structures. Applications normally use the derive through `catalejo::offset::Field`.

For a structure or union with named fields, the derive exposes field markers
as associated constants such as `Header::length`. These markers can be passed
to Catalejo's field projection APIs without calculating byte offsets by hand.

Because field projection produces typed access handles, the derive also
implements `Unassociated` for the enclosing type and requires every field to
satisfy that trait's validity contract.

Padding in the enclosing type is permitted and need not be initialized. Fields
must be aligned for projection, which the derive checks.

Generic types, enums, tuple structures, and unit structures are unsupported.
Use manual trait implementations when the derive cannot describe the layout,
and uphold the safety requirements of each trait.
