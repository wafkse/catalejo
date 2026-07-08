//! Derive macros for the `catalejo` crate.
#![forbid(
    clippy::alloc_instead_of_core,
    clippy::std_instead_of_core,
    clippy::std_instead_of_alloc,
    clippy::missing_const_for_fn,
    missing_unsafe_on_extern,
    missing_abi,
    missing_docs
)]

use std::collections::HashSet;

use proc_macro2::TokenStream;

use quote::{ToTokens, format_ident, quote};

use syn::{Data, DeriveInput, Fields};

/// Derive the `catalejo` [`Field`] trait for the named fields of a type.
///
/// For a `struct` or `union` with named fields, this reifies each field as a
/// distinct, default-constructible marker type that implements [`Field`] with its
/// `Structure` set to the annotated type and its offset resolved through
/// [`offset_of!`]. Each marker is also exposed as an inherent associated constant
/// on the annotated type, so that a field is named as `Structure::field`.
///
/// An [`Unassociated`] implementation is emitted for the annotated type, gated on
/// each of its field types being [`Unassociated`] as well. This gate is a
/// soundness requirement rather than a silent opt-out. The bound ranges over the
/// concrete field types, so deriving on a type that holds a non-[`Unassociated`]
/// field is a compile error, as stable Rust rejects unsatisfiable trivial bounds
/// (see rust-lang/rust#48214). Derive this only for types whose fields are all
/// [`Unassociated`].
///
/// A generic type is rejected. Bindgen does not emit generic types, and such a
/// type can implement [`Field`] by hand.
///
/// [`Field`]: catalejo::offset::Field
/// [`Unassociated`]: catalejo::offset::Unassociated
/// [`offset_of!`]: core::mem::offset_of
#[proc_macro_derive(Field)]
pub fn derive_field(target_input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let derive_input = syn::parse_macro_input!(target_input as DeriveInput);

    expand_field(derive_input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

/// Expand the [`Field`](catalejo::offset::Field) derive, or fail with a spanned error.
fn expand_field(derive_input: DeriveInput) -> syn::Result<TokenStream> {
    let DeriveInput {
        ident,
        generics,
        data,
        ..
    } = derive_input;

    if !generics.params.is_empty() {
        return Err(syn::Error::new_spanned(
            &ident,
            "`Field` cannot be derived for generic types",
        ));
    }

    let field_list = match data {
        Data::Struct(data_struct) => match data_struct.fields {
            Fields::Named(fields_named) => fields_named.named,
            Fields::Unnamed(..) | Fields::Unit => {
                return Err(syn::Error::new_spanned(
                    &ident,
                    "`Field` can only be derived for structs with named fields",
                ));
            }
        },
        Data::Union(data_union) => data_union.fields.named,
        Data::Enum(..) => {
            return Err(syn::Error::new_spanned(
                &ident,
                "`Field` cannot be derived for enums",
            ));
        }
    };

    let field_list = field_list
        .into_iter()
        .map(|field| {
            // NOTE: Carry the field's own doc attributes onto the generated marker and
            // constant, so hovering `Structure::field` surfaces what the field actually is.
            let doc_attr_list = field
                .attrs
                .into_iter()
                .filter(|attr| attr.path().is_ident("doc"))
                .collect::<Vec<_>>();

            let field_ident = field.ident.expect("a named field carries an identifier");

            (field_ident, field.ty, doc_attr_list)
        })
        .collect::<Vec<_>>();

    // NOTE: The markers live in a per-type module (named after the annotated type)
    // so that fields sharing a name across structures (e.g. `Elf32_Ehdr::e_type`
    // and `Elf64_Ehdr::e_type`) do not collide in the type namespace.
    let module_ident = format_ident!("{}", ident.to_string().to_lowercase());

    // NOTE: A trailing sentence links each marker back to the structure it describes.
    let marker_doc =
        format!("This is an auto-generated field marker of [`{ident}`](super::{ident}).");

    let markers = field_list.iter().map(|(field_ident, field_ty, doc_attr_list)| {
        quote! {
            #( #doc_attr_list )*
            #[doc = ""]
            #[doc = #marker_doc]
            #[derive(Debug, Clone, Copy, Default)]
            pub struct #field_ident;

            // SAFETY: A field marker is a zero-sized type with a single, always-valid
            // representation, it holds no bytes to tear or misinterpret.
            unsafe impl ::catalejo::prelude::Unassociated for #field_ident {}

            // SAFETY: `offset_of!` yields the true byte offset of `#field_ident` within
            // `Structure`, which is therefore in bounds of the structure, upholding the
            // `Field` contract that the returned offset lie within its structure.
            unsafe impl ::catalejo::prelude::Field for #field_ident {
                type Structure = super::#ident;

                type Value = #field_ty;

                #[inline]
                fn offset(_: impl ::core::borrow::Borrow<Self>) -> ::catalejo::prelude::Offset {
                    ::catalejo::prelude::Offset::byte(::core::mem::offset_of!(super::#ident, #field_ident))
                }
            }
        }
    });

    let const_item_list = field_list.iter().map(|(field_ident, _, doc_attr_list)| {
        quote! {
            #( #doc_attr_list )*
            #[allow(non_upper_case_globals)]
            pub const #field_ident: #module_ident::#field_ident = #module_ident::#field_ident;
        }
    });

    // NOTE: Duplicate `where` predicates are legal, but deduplicating the field types
    // keeps the emitted bound readable for the many repeated typedefs in bindgen-generated sources.
    let mut visited_set = HashSet::new();

    let bound_list = field_list
        .iter()
        .map(|(_, field_type, _)| field_type)
        .filter(|field_type| visited_set.insert(field_type.to_token_stream().to_string()))
        .map(|field_type| quote! { #field_type: ::catalejo::offset::Unassociated })
        .collect::<Vec<_>>();

    let where_clause = if bound_list.is_empty() {
        quote! {}
    } else {
        quote! { where #( #bound_list ),* }
    };

    Ok(quote! {
        #[doc(hidden)]
        pub mod #module_ident {
            #( #markers )*
        }

        impl #ident {
            #( #const_item_list )*
        }

        // SAFETY: For a `repr(C)` aggregate every byte belongs to some field. If each
        // field type is `Unassociated` (valid for every bit-pattern and tolerant of
        // tearing), the aggregate is too, and it round-trips through its byte form, and
        // padding contributes only always-valid bytes.
        unsafe impl ::catalejo::prelude::Unassociated for #ident #where_clause {}
    })
}
