use syn::{parse_quote, visit_mut::VisitMut, ItemUse, UseTree};

/// Removes the specified prefix from `use` statements.
///
/// If the prefix is `pkg`, then:
///  - `use pkg` -> `use {}`
///  - `use pkg::foo` -> `use foo`
///  - `use pkg::{foo, bar}` -> `use {foo, bar}`
pub struct UseTrimmer {
    /// The prefix to remove from `use` statements
    prefix: String,
}

impl UseTrimmer {
    pub fn with_prefix(prefix: String) -> Self {
        Self { prefix }
    }
}

impl VisitMut for UseTrimmer {
    fn visit_item_use_mut(&mut self, i: &mut ItemUse) {
        match i.tree {
            // `use pkg::...`
            // skip the prefix
            UseTree::Path(ref use_path) => {
                if use_path.ident.to_string().starts_with(&self.prefix) {
                    i.tree = use_path.tree.as_ref().clone();
                }
            }
            // `use pkg`
            // replace by `use {}`
            UseTree::Name(ref use_name) => {
                if use_name.ident.to_string().starts_with(&self.prefix) {
                    i.tree = parse_quote! { {} };
                }
            }
            _ => {}
        }

        // no need to visit further
        // syn::visit_mut::visit_item_use_mut(self, i);
    }
}
