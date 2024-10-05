use syn::visit_mut::VisitMut;

/// Removes parts of the code that include the specified attributes
pub struct Cleaner {
    pub attributes_to_remove: Vec<String>,
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

impl VisitMut for Cleaner {
    remove_attrs!(visit_item_fn_mut, ItemFn);
    remove_attrs!(visit_item_enum_mut, ItemEnum);
    remove_attrs!(visit_item_trait_mut, ItemTrait);
    remove_attrs!(visit_item_struct_mut, ItemStruct);
    remove_attrs!(visit_item_impl_mut, ItemImpl);
    remove_attrs!(visit_field_mut, Field);
}
