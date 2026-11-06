use std::process::Stdio;

use harmonia_protocol::types::DaemonStore;
use harmonia_store_remote::DaemonClient;
use tokio::process::Command;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut daemon = Command::new(std::env::args().nth(1).unwrap())
        .args(["daemon", "--stdio", "--store", "dummy://"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()?;
    let mut client = DaemonClient::builder()
        .connect(daemon.stdout.take().unwrap(), daemon.stdin.take().unwrap())
        .await?;
    let result = client.query_missing(&[]).await;
    daemon.start_kill()?;
    daemon.wait().await?;
    result?;
    Ok(())
}
