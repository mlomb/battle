use std::{error::Error, fs, path::PathBuf};
use syn::{parse_quote, visit_mut::VisitMut};

/// Recursively resolves the `use` and `extern crate` statements in the code,
/// effectively inlining all the code.
///
/// At the same time, it collecs all the files that were read to resolve the lines.
pub struct ModInliner {
    pub base_path: PathBuf,
    pub crate_name: String,
}

impl ModInliner {
    pub fn new(base_path: PathBuf, crate_name: String) -> Self {
        Self {
            base_path,
            crate_name,
        }
    }
}

impl ModInliner {
    fn expand_items(&self, items: &mut Vec<syn::Item>) {
        let mut new_items = vec![];
        for item in items.drain(..) {
            self.expand_item(item, &mut new_items);
        }
        *items = new_items;
    }

    fn expand_item(&self, item: syn::Item, new_items: &mut Vec<syn::Item>) {
        match item {
            syn::Item::ExternCrate(ref item) => {
                if item.ident == self.crate_name {
                    /*eprintln!(
                        "expanding crate {} in {}",
                        self.crate_name,
                        self.base_path.to_str().unwrap()
                    );*/
                    let code = fs::read_to_string(&self.base_path.join("lib.rs"))
                        .expect("failed to read lib.rs");
                    let lib = syn::parse_file(&code).expect("failed to parse lib.rs");
                    new_items.extend(lib.items);
                }
            }
            // keep items as is
            _ => new_items.push(item),
        }
    }

    fn expand_mods(&self, item: &mut syn::ItemMod) {
        if item.content.is_some() {
            return;
        }
        let name = item.ident.to_string();
        let other_base_path = self.base_path.join(&name);

        let (base_path, code) = vec![
            (self.base_path.clone(), format!("{}.rs", name)),
            (other_base_path, String::from("mod.rs")),
        ]
        .into_iter()
        .flat_map(|(base_path, file_name)| {
            fs::read_to_string(&base_path.join(file_name)).map(|code| (base_path, code))
        })
        .next()
        .expect("mod not found");
        //eprintln!("expanding mod {} in {}", name, base_path.to_str().unwrap());

        if let Ok(mut file) = syn::parse_file(&code) {
            ModInliner {
                base_path,
                crate_name: self.crate_name.clone(),
            }
            .visit_file_mut(&mut file);
            item.content = Some((Default::default(), file.items));
        } else {
            eprintln!("failed to parse file {}", name);
        }
    }

    fn resolve(&mut self, name: &String) -> Result<syn::File, Box<dyn Error>> {
        let other_base_path = self.base_path.join(&name);

        let (base_path, code) = vec![
            (self.base_path.clone(), format!("{}.rs", name)),
            (other_base_path, String::from("mod.rs")),
        ]
        .into_iter()
        .flat_map(|(base_path, file_name)| {
            fs::read_to_string(&base_path.join(file_name)).map(|code| (base_path, code))
        })
        .next()
        .ok_or(format!("mod not found: {}", name))?;

        syn::parse_file(&code).map_err(|e| e.into())
    }
}

impl VisitMut for ModInliner {
    //fn visit_file_mut(&mut self, file: &mut syn::File) {
    //    // syn::visit_mut::visit_file_mut(self, file);
    //
    //    for it in &mut file.attrs {
    //        self.visit_attribute_mut(it)
    //    }
    //
    //    self.expand_items(&mut file.items);
    //
    //    for it in &mut file.items {
    //        self.visit_item_mut(it)
    //    }
    //}

    // fn visit_item_mod_mut(&mut self, item: &mut syn::ItemMod) {
    //     for it in &mut item.attrs {
    //         self.visit_attribute_mut(it)
    //     }
    //     self.visit_visibility_mut(&mut item.vis);
    //     self.visit_ident_mut(&mut item.ident);
    //     self.expand_mods(item);
    //     if let Some(ref mut it) = item.content {
    //         for it in &mut (it).1 {
    //             self.visit_item_mut(it);
    //         }
    //     }
    // }

    fn visit_item_mod_mut(&mut self, i: &mut syn::ItemMod) {
        // check that the mod is not defined inline (not mod m { ... })
        if i.content.is_none() {
            // resolve mod recursively
            let file = self.resolve(&i.ident.to_string());

            if let Ok(mut file) = file {
                self.visit_file_mut(&mut file);

                // Note: file attributes are being dropped (shebang)
                i.content = Some((Default::default(), file.items));
            } else {
                i.attrs.push(parse_quote! { #[doc="Failed to resolve"] });
                i.content = Some((Default::default(), vec![]));
            }
        }
    }

    fn visit_item_use_mut(&mut self, i: &mut syn::ItemUse) {
        //
        println!("visiting use");
    }
}
