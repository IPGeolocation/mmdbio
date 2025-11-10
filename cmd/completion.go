package cmd

import (
	"os"

	"github.com/spf13/cobra"
)

var completionCmd = &cobra.Command{
	Use:   "completion [bash|zsh|fish|powershell]",
	Short: "Generate completion script",
	Long: `To load completions:

Bash:

  $ source <(yourapp completion bash)

  # To load completions for each session, execute once:
  # Linux:
  $ yourapp completion bash > /etc/bash_completion.d/yourapp
  # macOS:
  $ yourapp completion bash > /usr/local/etc/bash_completion.d/yourapp

Zsh:

  $ echo "autoload -U compinit; compinit" >> ~/.zshrc
  $ yourapp completion zsh > "${fpath[1]}/_yourapp"

Fish:

  $ yourapp completion fish | source
  $ yourapp completion fish > ~/.config/fish/completions/yourapp.fish

PowerShell:

  PS> yourapp completion powershell | Out-String | Invoke-Expression
  PS> yourapp completion powershell > yourapp.ps1
`,
	DisableFlagsInUseLine: true,
	Args:                  cobra.ExactValidArgs(1),
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
