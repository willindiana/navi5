#!/usr/bin/env ruby
# Navi5 — offline master workflow safety checks.
# SPDX-License-Identifier: GPL-3.0-or-later
require 'yaml'
root = File.expand_path('..', __dir__)
load_workflow = ->(path) { YAML.load_file(File.join(root, path)) }
github = load_workflow.call('.github/workflows/release-build.yml')
forgejo = load_workflow.call('.forgejo/workflows/release-build.yml')
store = load_workflow.call('.github/workflows/store-verify.yml')
[github, forgejo, store].each do |workflow|
  events = workflow.fetch('on', workflow[true]) # Psych YAML 1.1 reads on as true.
  raise 'unexpected event' unless (events.keys - %w[push pull_request workflow_dispatch]).empty?
  raise 'manual dispatch missing' unless events.key?('workflow_dispatch')
  events.each do |event, config|
    next if event == 'workflow_dispatch'
    raise 'wrong base' unless config.fetch('branches') == ['master']
  end
  workflow.fetch('jobs').each_value do |job|
    raise 'runner changed' unless job.fetch('runs-on') == 'ubuntu-24.04'
    checkout = job.fetch('steps').find { |step| step['uses']&.include?('actions/checkout@') }
    raise 'checkout credentials persisted' unless checkout.fetch('with').fetch('persist-credentials') == false
    raise 'beta guard retained' if job.fetch('if').include?('release/beta')
    raise 'master guard missing' unless job.fetch('if').include?('refs/heads/master')
  end
end
raise 'GitHub PR missing' unless github.fetch('on', github[true]).key?('pull_request')
raise 'Forgejo PR execution enabled' if forgejo.fetch('on', forgejo[true]).key?('pull_request')
raise 'store not manual-only' unless store.fetch('on', store[true]).keys == ['workflow_dispatch']
[github, store].each do |workflow|
  raise 'GitHub write permission' unless workflow.fetch('permissions') == {'contents' => 'read'}
end
puts 'Master triggers, manual store gate, hosted read-only checkout and no Forgejo PR execution passed'
