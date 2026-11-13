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

/// Generate typed field markers for a structure or union with named fields.
///
/// Each field becomes an associated constant such as `Header::length`. Its
/// marker implements `Field`, identifying the enclosing type, field type, and
/// byte offset computed by [`offset_of!`]. Field documentation is copied to the
/// marker and associated constant.
///
/// Because projection produces typed foreign handles, the derive also
/// implements `Unassociated` for the enclosing type and requires that trait on
/// every field. A field that cannot satisfy the bound causes compilation to fail.
///
/// # Layout
///
/// Padding is permitted and need not be initialized. The derive checks that
/// fields are aligned for projection.
///
/// # Supported layouts
///
/// Generic types, enums, tuple structures, and unit structures are rejected.
/// Types outside these supported forms need manual trait implementations.
///
/// [`offset_of!`]: core::mem::offset_of
#[proc_macro_derive(Field)]
pub fn derive_field(target_input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    let derive_input = syn::parse_macro_input!(target_input as DeriveInput);

    expand_field(derive_input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

/// Expand the `Field` derive, or fail with a spanned error.
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

    // NOTE: The markers live in a per-type module so that fields sharing a name across
    // structures (e.g. `Elf32_Ehdr::e_type` and `Elf64_Ehdr::e_type`) do not collide in
    // the type namespace. The `__`-affixed name keeps the module distinct from the
    // annotated type itself, whose own name may already be snake_case (e.g. `link_map`).
    let module_ident = format_ident!("__{}_fields", ident);

    // NOTE: A trailing sentence links each marker back to the structure it describes.
    let marker_doc =
        format!("This is an auto-generated field marker of [`{ident}`](super::{ident}).");

    let markers = field_list.iter().map(|(field_ident, field_ty, doc_attr_list)| {
        quote! {
            #( #doc_attr_list )*
            #[doc = ""]
            #[doc = #marker_doc]
            #[allow(
                non_camel_case_types,
                reason = "generated marker types preserve the foreign field name"
            )]
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
                    ::catalejo::prelude::Offset::byte(::core::mem::offset_of!(super::#ident, #field_ident) as ::catalejo::ffi::binding::virtual_size_t)
                }
            }

            const _: () = {
                assert!(
                    ::core::mem::align_of::<#ident>()
                        >= ::core::mem::align_of::<#field_ty>()
                );

                assert!(
                    ::core::mem::offset_of!(#ident, #field_ident)
                        % ::core::mem::align_of::<#field_ty>()
                        == 0
                );
            };
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
        #[allow(
            non_snake_case,
            reason = "the generated module preserves the enclosing foreign structure name"
        )]
        pub mod #module_ident {
            use super::*;

            #( #markers )*
        }

        impl #ident {
            #( #const_item_list )*
        }

        // SAFETY: Each field type is `Unassociated`, so value-bearing fields remain
        // valid under arbitrary bit patterns and tearing. Any padding need not be
        // initialized and imposes no additional validity requirements.
        unsafe impl ::catalejo::prelude::Unassociated for #ident #where_clause {}
    })
}
