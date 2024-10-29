use syn::{visit_mut::VisitMut, Attribute};

/// Expands parameter definitions to values read from standard arguments
pub struct ParameterExpander {}

impl ParameterExpander {
    /// Finds and extract the content of a #[doc] attribute
    ///
    /// ```#[doc = "Hello"] → Some("Hello")```
    fn extract_doc_content(&self, attrs: &Vec<Attribute>) -> Option<String> {
        for attr in attrs {
            if attr.path().is_ident("doc") {
                if let syn::Meta::NameValue(ref meta) = attr.meta {
                    if let syn::Expr::Lit(syn::ExprLit { lit, attrs: _ }) = &meta.value {
                        if let syn::Lit::Str(ref content) = lit {
                            return Some(content.value().to_string());
                        }
                    }
                }
            }
        }
        None
    }

    fn is_parameter(&self, attrs: &Vec<Attribute>) -> bool {
        if let Some(doc) = self.extract_doc_content(attrs) {
            return doc.trim() == "RealParam";
        }
        false
    }
}

impl VisitMut for ParameterExpander {
    fn visit_item_const_mut(&mut self, i: &mut syn::ItemConst) {
        if self.is_parameter(&i.attrs) {
            i.expr = syn::parse_quote! {
                LazyCell::new(|| {
                    std::env::args()
                        .skip(1)
                        .collect::<Vec<_>>()
                        .chunks_exact(2)
                        .find(|item| item[0] == 123)
                        .map(|item| item[1].parse().unwrap())
                        .unwrap_or_else(|| 42.0)
                })
            };
        }
    }
}
