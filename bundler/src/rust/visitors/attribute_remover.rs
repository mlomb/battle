use syn::visit_mut::VisitMut;

/// Removes specified attributes from a syntax tree
pub struct AttributeRemover {
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
}
