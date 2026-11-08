use std::collections::HashMap;
use syn::{visit_mut::VisitMut, Expr};

/// Replaces the value of the `const` items with the given names.
pub struct ConstOverrider(pub HashMap<String, Expr>);

impl VisitMut for ConstOverrider {
    fn visit_item_const_mut(&mut self, i: &mut syn::ItemConst) {
        if let Some(value) = self.0.get(&i.ident.to_string()) {
            *i.expr = value.clone();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ConstOverrider;
    use quote::quote;
    use std::collections::HashMap;
    use syn::{visit_mut::VisitMut, File};

    #[test]
    fn replaces_consts_in_modules() {
        let mut file: File = syn::parse_quote! {
            const DEPTH: usize = 6;
            const OTHER: usize = 1;
            mod search {
                const MOVE: &str = "UP";
            }
        };
        let values = HashMap::from([
            ("DEPTH".to_string(), syn::parse_quote!(4)),
            ("MOVE".to_string(), syn::parse_quote!("LEFT")),
        ]);

        ConstOverrider(values).visit_file_mut(&mut file);

        let expected: File = syn::parse_quote! {
            const DEPTH: usize = 4;
            const OTHER: usize = 1;
            mod search {
                const MOVE: &str = "LEFT";
            }
        };
        assert_eq!(quote!(#file).to_string(), quote!(#expected).to_string());
    }
}
