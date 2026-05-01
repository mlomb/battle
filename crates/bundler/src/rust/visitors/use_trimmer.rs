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
            UseTree::Path(ref use_path) if use_path.ident.to_string().starts_with(&self.prefix) => {
                i.tree = use_path.tree.as_ref().clone();
            }
            // `use pkg`
            // replace by `use {}`
            UseTree::Name(ref use_name) if use_name.ident.to_string().starts_with(&self.prefix) => {
                i.tree = parse_quote! { {} };
            }
            _ => {}
        }

        // no need to visit further
        // syn::visit_mut::visit_item_use_mut(self, i);
    }
}

#[cfg(test)]
mod tests {
    use super::UseTrimmer;
    use syn::{visit_mut::VisitMut, File};

    #[test]
    fn trim_matching_use_prefixes() {
        let mut input: File = syn::parse_str(
            r#"
            use mypkg::foo::Bar;
            use mypkg;
            use other::foo;
            use other;
            use foo as bar;
            use mypkg::{ a, b };
            use mypkg_extra::foo;
            "#,
        )
        .expect("parse input");

        let expected: File = syn::parse_str(
            r#"
            use foo::Bar;
            use {};
            use other::foo;
            use other;
            use foo as bar;
            use { a, b };
            use foo;
            "#,
        )
        .expect("parse expected");

        UseTrimmer::with_prefix("mypkg".into()).visit_file_mut(&mut input);

        assert_eq!(input, expected);
    }
}
