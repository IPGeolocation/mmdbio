package cmd

import (
	"os"

	"github.com/spf13/cobra"
)

var completionCmd = &cobra.Command{
	Use:   "completion [bash|zsh|fish|powershell]",
	Short: "Generate shell completion scripts",
	Long: `To load completions:

Bash:

  $ source <(mmdbio completion bash)

  # To load completions for each session, execute once:
  # Linux:
  $ mmdbio completion bash > /etc/bash_completion.d/mmdbio
  # macOS:
  $ mmdbio completion bash > /usr/local/etc/bash_completion.d/mmdbio

Zsh:

  $ echo "autoload -U compinit; compinit" >> ~/.zshrc
  $ mmdbio completion zsh > "${fpath[1]}/_mmdbio"

Fish:

  $ mmdbio completion fish | source
  $ mmdbio completion fish > ~/.config/fish/completions/mmdbio.fish

PowerShell:

  PS> mmdbio completion powershell | Out-String | Invoke-Expression
  PS> mmdbio completion powershell > mmdbio.ps1
`,
	DisableFlagsInUseLine: true,
	Args:                  cobra.MatchAll(cobra.ExactArgs(1), cobra.OnlyValidArgs),
	ValidArgs:             []string{"bash", "zsh", "fish", "powershell"},
	Run: func(cmd *cobra.Command, args []string) {
		switch args[0] {
		case "bash":
			cmd.Root().GenBashCompletion(os.Stdout)
		case "zsh":
			cmd.Root().GenZshCompletion(os.Stdout)
		case "fish":
			cmd.Root().GenFishCompletion(os.Stdout, true)
		case "powershell":
			cmd.Root().GenPowerShellCompletionWithDesc(os.Stdout)
		}
	},
}

func init() {
	rootCmd.AddCommand(completionCmd)
}
