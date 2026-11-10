# Take-Home Assignment: An OCPP Charger Running on QEMU

## The task

Using the attached OCPP specification and test case document, build **charge point firmware that runs on QEMU**, and submit it as a GitHub repository.

There is no real hardware. Everything a charger deals with in the real world (the vehicle, the cable, the card, the power) should be simulated in whatever way you think is appropriate. A real charger, once connected to a vehicle by cable, exchanges state with it, and what the charger reports to the server depends on the outcome. How far you go in simulating that is up to you.

- **Deadline: Monday, 16 November 2026, 12:00 noon (KST)**

## What we provide

- The OCPP **[1.6J]** specification
- The test case document
- A channel for questions: **[myung@starkoff.co.kr / junochoi@starkoff.co.kr]**

We do not provide a central system (CSMS).

## What is fixed

These are the only fixed conditions. Anything not listed here is your decision.

1. It must run on QEMU, and it must not be an ordinary program running on Linux. Assume a microcontroller-class environment like the one inside a real charger.
2. We will connect your implementation to our automated test tool and verify it **according to the test case document**. We therefore need to be able to change the server address and the charge point ID.
3. We will build and run it on a clean **[Ubuntu 22.04]** machine, following only what your repository tells us.
4. You do not need to pass every test case. You do need to know what works and what does not, and tell us. Something you say works that fails on our side is scored lower than something you say does not work.

## Submission

- Submit a **GitHub repository**. No zip files, bundles, or other hosting services.
- The repository may be private. If it is, invite **[myung2-starkoff and juno-choi-starkoff]** as a collaborator before the deadline.
- Send the repository URL to **[myung@starkoff.co.kr / junochoi@starkoff.co.kr]** before the deadline.
- We evaluate the state of the default branch at the deadline. Commits pushed after that are ignored.

## What is allowed

- Public material, open source, and AI tools are all allowed. Tell us what you used and how. Using them costs you nothing. Hiding it does.
- Taking someone else's charge point implementation wholesale, or having another person write the code for you, is not allowed.

## What we look at

- Does it run in our environment?
- Did you read the specification properly?
- How did you check your own work before submitting it?
- How does it behave when things do not go as expected?
- What decisions did you make, and can you explain why?
- Do you know your own limits?

## After submission

We will let you know the result within **[5 days]**.
