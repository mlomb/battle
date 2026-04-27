use syn::visit_mut::VisitMut;

/// Removes the specified attributes.
pub struct AttributeRemover {
    /// The attributes to remove
    attributes_to_remove: Vec<String>,
}

impl AttributeRemover {
    pub fn new() -> Self {
        Self {
            attributes_to_remove: vec![],
        }
    }

    pub fn with_attribute<T: ToString>(mut self, attribute: T) -> Self {
        self.attributes_to_remove.push(attribute.to_string());
        self
    }
}

macro_rules! remove_attrs {
    ($fn_name: ident, $type_name: ident) => {
        fn $fn_name(&mut self, i: &mut syn::$type_name) {
            i.attrs.retain(|attr| {
                !self
                    .attributes_to_remove
                    .iter()
                    .any(|attr_name| attr.path().is_ident(attr_name))
            });
            syn::visit_mut::$fn_name(self, i)
        }
    };
}

impl VisitMut for AttributeRemover {
    remove_attrs!(visit_item_fn_mut, ItemFn);
    remove_attrs!(visit_item_const_mut, ItemConst);
    remove_attrs!(visit_item_enum_mut, ItemEnum);
    remove_attrs!(visit_item_trait_mut, ItemTrait);
    remove_attrs!(visit_item_struct_mut, ItemStruct);
    remove_attrs!(visit_item_impl_mut, ItemImpl);
    remove_attrs!(visit_field_mut, Field);
    remove_attrs!(visit_variant_mut, Variant);
    remove_attrs!(visit_impl_item_fn_mut, ImplItemFn);
}

#[cfg(test)]
mod tests {
    use super::AttributeRemover;
    use syn::{visit_mut::VisitMut, File, Item};

    #[test]
    fn remove_some_attributes() {
        let mut input: File = syn::parse_str(
            r#"
            #[doc = "doc on struct"]
            #[derive(Debug)]
            pub struct Foo {
                #[doc = "doc on field"]
                pub bar: i32,
            }

            #[wasm_bindgen]
            #[inline]
            pub fn boom() {}
        "#,
        )
        .expect("parse input");

        let expected_src: File = syn::parse_str(
            r#"
            #[derive(Debug)]
            pub struct Foo {
                pub bar: i32,
            }

            #[inline]
            pub fn boom() {}
        "#,
        )
        .expect("parse expected");

        AttributeRemover::new()
            .with_attribute("doc")
            .with_attribute("wasm_bindgen")
            .visit_file_mut(&mut input);

        assert_eq!(input, expected_src);
    }
}
