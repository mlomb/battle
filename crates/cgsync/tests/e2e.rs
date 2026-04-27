#[path = "client.rs"]
mod client;

use build_fs_tree::{dir, file, Build, FileSystemTree, MergeableFileSystemTree};
use client::CGExtensionClient;
use serial_test::serial;
use std::time::Duration;
use tempfile::TempDir;

const NO_UPDATE_TIMEOUT: Duration = Duration::from_millis(500);

fn prepare_fixture(tree: FileSystemTree<&str, &str>) -> TempDir {
    let tmp = tempfile::tempdir().expect("tempdir");
    let root = tmp.path();
    let tree: MergeableFileSystemTree<_, _> = MergeableFileSystemTree::from(tree);
    tree.build(root).expect("build fs fixture");
    tmp
}

/// Touching an included file sends an update.
#[tokio::test]
#[serial]
async fn file_change_updates_code_over_websocket() {
    let tmp = prepare_fixture(dir! {
        "main.cpp" => file!("// marker: v1\nint main() { return 0; }\n")
    });
    let main_cpp = tmp.path().join("main.cpp");

    let mut client = CGExtensionClient::start(tmp.path()).await;
    client.assert_handshake().await;

    // initial code bundle
    let code = client.next_update_code().await;
    assert!(code.contains("marker: v1"));

    // trigger a file change
    std::fs::write(main_cpp, "// marker: v2\nint main() { return 42; }\n").unwrap();

    // updated bundle should arrive
    let code = client.next_update_code().await;
    assert!(code.contains("marker: v2"));
    assert!(!code.contains("marker: v1"));
}

/// Touching a file that is NOT included must not send any bundle.
#[tokio::test]
#[serial]
async fn untracked_file_change_does_not_trigger_update() {
    let tmp = prepare_fixture(dir! {
        "main.cpp" => file!("// main\nint main() { return 0; }\n"),
        "untracked.h" => file!("// untracked: original\n")
    });

    let mut client = CGExtensionClient::start(tmp.path()).await;
    client.assert_handshake().await;

    // consume the initial bundle (main.cpp only, untracked.h not referenced)
    let code = client.next_update_code().await;
    assert!(code.contains("main"));
    assert!(!code.contains("untracked"));

    // touch the untracked file
    std::fs::write(tmp.path().join("untracked.h"), "// untracked: changed\n")
        .expect("write untracked.h");

    // no bundle should arrive within a generous window
    assert!(client
        .try_next_update_code(NO_UPDATE_TIMEOUT)
        .await
        .is_none(),);
}

/// Once main.cpp is edited to #include a previously-untracked file,
/// subsequent changes to that file must trigger updates.
#[tokio::test]
#[serial]
async fn newly_included_file_becomes_tracked() {
    // Use all-caps sentinels that cannot appear in the C++ pragma header.
    let tmp = prepare_fixture(dir! {
        "main.cpp" => file!("// initial\nint main() { return 0; }\n"),
        "helper.h" => file!("// helper v1\n")
    });
    let main_cpp = tmp.path().join("main.cpp");
    let helper_h = tmp.path().join("helper.h");

    let mut client = CGExtensionClient::start(tmp.path()).await;
    client.assert_handshake().await;

    // initial bundle — helper.h is not referenced
    let code = client.next_update_code().await;
    assert!(code.contains("initial"));
    assert!(!code.contains("helper v1"));

    // touching helper.h should NOT trigger an update yet
    std::fs::write(&helper_h, "// helper v2\n").unwrap();
    assert!(client
        .try_next_update_code(NO_UPDATE_TIMEOUT)
        .await
        .is_none());

    // edit main.cpp to include helper.h — this change triggers an update
    std::fs::write(
        &main_cpp,
        "// updated\n#include \"helper.h\"\nint main() { return 0; }\n",
    )
    .unwrap();
    let code = client.next_update_code().await;
    assert!(!code.contains("initial"));
    assert!(code.contains("updated"));
    assert!(code.contains("helper v2"));

    // now helper.h is tracked — further changes to it must trigger updates
    std::fs::write(&helper_h, "// helper v3\n").unwrap();
    let code = client.next_update_code().await;
    assert!(code.contains("updated"));
    assert!(code.contains("helper v3"));
}
