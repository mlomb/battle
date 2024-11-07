use proc_macro2::TokenTree;
use syn::{visit_mut::VisitMut, Item, Meta};

/// Removes test functions and modules from a syntax tree
///
/// ```ignore
/// #[test]
/// fn test() { ... }
///
/// #[cfg(test)]
/// mod tests { ... }
/// ```
pub struct TestRemover {}

impl TestRemover {
    pub fn new() -> Self {
        Self {}
    }
}

impl VisitMut for TestRemover {
    fn visit_item_mod_mut(&mut self, i: &mut syn::ItemMod) {
        if let Some(content) = &mut i.content {
            content.1.retain(|item| {
                match item {
                    // remove #[test]
                    Item::Fn(f) => !f.attrs.iter().any(is_test_attribute),
                    // remove #[cfg(test)]
                    Item::Mod(m) => !m.attrs.iter().any(is_test_attribute),
                    // keep everything else
                    _ => true,
                }
            });
        }

        syn::visit_mut::visit_item_mod_mut(self, i)
    }
}

fn is_test_attribute(attr: &syn::Attribute) -> bool {
    match attr.meta {
        // #[test]
        Meta::Path(ref path) => path.is_ident("test"),
        // #[cfg(test)]
        Meta::List(ref list) => {
            list.path.is_ident("cfg")
                && match list.tokens.clone().into_iter().next() {
                    Some(TokenTree::Ident(i)) => i.to_string() == "test",
                    _ => false,
                }
        }
        // anything else
        _ => false,
    }
}
