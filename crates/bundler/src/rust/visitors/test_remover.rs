use proc_macro2::TokenTree;
use syn::{visit_mut::VisitMut, Item, Meta};

/// Removes modules and functions marked as tests.
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

#[cfg(test)]
mod tests {
    use super::TestRemover;
    use syn::{visit_mut::VisitMut, File};

    #[test]
    fn remove_test_only_items() {
        let mut input: File = syn::parse_str(
            r#"
            mod outer {
                mod decl_only;

                #[test]
                fn it_works() {}

                #[cfg(test)]
                mod tests {
                    fn helper() {}
                }

                #[cfg()]
                fn cfg_empty_paren() {}

                #[cfg((test))]
                fn cfg_group_first_token() {}

                #[doc = "stay"]
                #[cfg(target_os = "linux")]
                fn regular() {}

                mod normal {}

                struct Foo;

                const BAZ: i32 = 1;
            }
            "#,
        )
        .expect("parse input");

        let expected: File = syn::parse_str(
            r#"
            mod outer {
                mod decl_only;

                #[cfg()]
                fn cfg_empty_paren() {}

                #[cfg((test))]
                fn cfg_group_first_token() {}

                #[doc = "stay"]
                #[cfg(target_os = "linux")]
                fn regular() {}

                mod normal {}

                struct Foo;

                const BAZ: i32 = 1;
            }
            "#,
        )
        .expect("parse expected");

        TestRemover::new().visit_file_mut(&mut input);

        assert_eq!(input, expected);
    }
}
